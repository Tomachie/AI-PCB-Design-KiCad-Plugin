// tweb - Tomachie web client, KiCad side (Linux port).
//
// Direct port of upstream tweb.cpp (Windows) to Linux. Wire behavior is
// unchanged: same KiCad IPC protobuf exchange, same sheet-walk, same ZIP
// layout, same /stage multipart upload. Only the OS layer differs:
//   - WinHTTP        -> curl CLI subprocess (no extra dev packages needed)
//   - LoadLibrary    -> dlopen("libnng.so.1") / direct -lz link
//   - Win32 dialog   -> zenity forms; GTK through python3 where zenity is
//                       absent (KiCad's Flatpak); console when neither
//   - ShellExecute   -> xdg-open
//   - APPDATA/TEMP   -> XDG_CONFIG_HOME / TMPDIR
//   - GetAsyncKeyState(Shift) is unavailable to a Linux plugin process:
//     run `tweb --settings` in a terminal to change the stored address.

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>

#include <zlib.h>

#include "string_table.h"

namespace fs = std::filesystem;

enum { ICON_ERROR = 1, ICON_WARNING = 2, ICON_INFO = 3 };

static bool g_verbose = false;

// Absolute path of this executable (via /proc/self/exe).
static std::string ownExePath()
{
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return std::string();
    buf[n] = '\0';
    return std::string(buf, size_t(n));
}

static std::string exeDir()
{
    std::string p = ownExePath();
    size_t i = p.find_last_of('/');
    return (i == std::string::npos) ? std::string() : p.substr(0, i);
}

static std::string logPath()
{
    std::string d = exeDir();
    if (!d.empty())
    {
        // Plugin dir is normally writable (~/.local/share/kicad/...).
        // If not, fall back to $TMPDIR below at write time.
        return d + "/tweb_log.txt";
    }
    const char* t = getenv("TMPDIR");
    return std::string(t && *t ? t : "/tmp") + "/tweb_log.txt";
}

static void logLine(const std::string& text)
{
    std::string lp = logPath();
    FILE* fh = fopen(lp.c_str(), "a");
    if (!fh)
    {
        const char* t = getenv("TMPDIR");
        lp = std::string(t && *t ? t : "/tmp") + "/tweb_log.txt";
        fh = fopen(lp.c_str(), "a");
        if (!fh)
            return;
    }
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "[%02d:%02d:%02d] ",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    std::string line = std::string(stamp) + text + "\n";
    fwrite(line.data(), 1, line.size(), fh);
    fclose(fh);
}

static bool guiAvailable()
{
    return getenv("WAYLAND_DISPLAY") != NULL || getenv("DISPLAY") != NULL;
}

static bool haveProg(const char* name)
{
    std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return system(cmd.c_str()) == 0;
}

static int runCapture(const std::vector<std::string>& args, const std::string& stdinData,
                      std::string& output, bool includeStderr = false);

// The subset of zenity this client uses (--info/--warning/--error, --forms
// with --add-entry, --title, --text, --ok-label, --cancel-label), drawn with
// GTK 3 through Python.  KiCad's Flatpak sandbox has python3 with GTK but no
// zenity, so without this the first-run dialog could never appear there.
// Same arguments, same output (form fields joined by '|'), same exit codes.
static const char* GTK_DIALOG_PY = R"PY(
import sys, gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk
a = sys.argv[1:]; o = {}; entries = []; kind = "info"; i = 0
while i < len(a):
    s = a[i]; i += 1
    if s in ("--info", "--warning", "--error", "--forms"):
        kind = s[2:]
    elif s == "--text" and i < len(a):
        o["text"] = a[i]; i += 1
    elif s.startswith("--add-entry="):
        entries.append(s[len("--add-entry="):])
    elif s.startswith("--") and "=" in s:
        n, v = s[2:].split("=", 1); o[n] = v
if kind == "forms":
    d = Gtk.Dialog(title=o.get("title", ""))
    d.add_button(o.get("cancel-label", "Cancel"), Gtk.ResponseType.CANCEL)
    d.add_button(o.get("ok-label", "OK"), Gtk.ResponseType.OK)
    box = d.get_content_area(); box.set_spacing(8); box.set_border_width(12)
    t = Gtk.Label(label=o.get("text", "")); t.set_line_wrap(True)
    t.set_max_width_chars(100); t.set_selectable(True); t.set_xalign(0); box.add(t)
    d.set_resizable(False)
    fields = []
    for e in entries:
        box.add(Gtk.Label(label=e, xalign=0))
        f = Gtk.Entry(); f.set_activates_default(True); box.add(f); fields.append(f)
    d.set_default_response(Gtk.ResponseType.OK); d.show_all()
    if fields: fields[0].grab_focus()
    if d.run() != Gtk.ResponseType.OK: sys.exit(1)
    print("|".join(f.get_text() for f in fields)); sys.exit(0)
types = {"info": Gtk.MessageType.INFO, "warning": Gtk.MessageType.WARNING,
         "error": Gtk.MessageType.ERROR}
d = Gtk.MessageDialog(message_type=types.get(kind, Gtk.MessageType.INFO),
                      buttons=Gtk.ButtonsType.OK, text=o.get("text", ""))
d.set_title(o.get("title", "")); d.run(); sys.exit(0)
)PY";

// The program that draws dialogs, as the start of an argv: zenity, else the
// GTK script above, else empty (no display, or neither is available).
static const std::vector<std::string>& dialogCommand()
{
    static bool done = false;
    static std::vector<std::string> cmd;
    if (done)
        return cmd;
    done = true;
    if (!guiAvailable())
        return cmd;
    if (haveProg("zenity"))
        cmd = { "zenity" };
    else if (system("python3 -c 'import gi; gi.require_version(\"Gtk\", \"3.0\");"
                    " from gi.repository import Gtk' >/dev/null 2>&1") == 0)
        cmd = { "python3", "-c", GTK_DIALOG_PY };
    return cmd;
}

// Best-effort GUI/console notice. Always logged; the success report only
// shows with --verbose (same policy as the Windows build).
static void finish(const std::string& text, int icon)
{
    logLine("RESULT:\n" + text + "\n----");
    if (icon == ICON_INFO && !g_verbose)
        return;
    std::vector<std::string> args = dialogCommand();
    if (!args.empty())
    {
        // Returns when the user dismisses it, the same contract as MessageBox.
        // runCapture gives the dialog /dev/null as stdin.
        args.push_back((icon == ICON_ERROR) ? "--error" : (icon == ICON_WARNING) ? "--warning" : "--info");
        args.push_back("--title=Tomachie");
        args.push_back("--no-wrap");
        args.push_back("--text");
        args.push_back(text);
        std::string ignored;
        runCapture(args, std::string(), ignored);
        return;
    }
    fprintf(stderr, "Tomachie: %s\n", text.c_str());
}

// ---------------- protobuf (unchanged from upstream) ----------------
static void putVarint(std::string& o, uint64_t v)
{
    while (v >= 0x80) { o.push_back(char((v & 0x7F) | 0x80)); v >>= 7; }
    o.push_back(char(v));
}
static void putTag(std::string& o, int f, int w) { putVarint(o, (uint64_t(f) << 3) | uint64_t(w)); }
static void putBytes(std::string& o, int f, const std::string& b)
{
    putTag(o, f, 2); putVarint(o, b.size()); o += b;
}
static void putEnum(std::string& o, int f, uint64_t v) { putTag(o, f, 0); putVarint(o, v); }

struct Reader
{
    const uint8_t* p;
    const uint8_t* end;
    explicit Reader(const std::string& s)
        : p(reinterpret_cast<const uint8_t*>(s.data())), end(p + s.size()) {}
    size_t remaining() const { return size_t(end - p); }

    bool varint(uint64_t& v)
    {
        v = 0; int shift = 0;
        while (p < end)
        {
            uint8_t c = *p++;
            v |= uint64_t(c & 0x7F) << shift;
            if (!(c & 0x80)) return true;
            shift += 7;
            if (shift > 63) return false;
        }
        return false;
    }

    bool next(int& field, int& wire, std::string& payload)
    {
        if (p >= end) return false;
        uint64_t tag;
        if (!varint(tag)) return false;
        field = int(tag >> 3); wire = int(tag & 7);
        if (field == 0) return false;
        if (wire == 2)
        {
            uint64_t n;
            if (!varint(n) || n > uint64_t(remaining())) return false;
            payload.assign(reinterpret_cast<const char*>(p), size_t(n));
            p += size_t(n);
        }
        else if (wire == 0)
        {
            uint64_t v;
            if (!varint(v)) return false;
            payload.assign(1, char(v & 0xFF));
        }
        else if (wire == 5) { if (remaining() < 4) return false; p += 4; payload.clear(); }
        else if (wire == 1) { if (remaining() < 8) return false; p += 8; payload.clear(); }
        else return false;
        return true;
    }
};

// ---------------- nng (runtime-loaded, as upstream does with nng.dll) ----------------
typedef uint32_t nng_socket_t;
typedef int         (*fn_req0_open)(nng_socket_t*);
typedef int         (*fn_dial)(nng_socket_t, const char*, void*, int);
typedef int         (*fn_send)(nng_socket_t, void*, size_t, int);
typedef int         (*fn_recv)(nng_socket_t, void*, size_t*, int);
typedef int         (*fn_close)(nng_socket_t);
typedef void        (*fn_free)(void*, size_t);
typedef const char* (*fn_strerror)(int);
typedef int         (*fn_set_ms)(nng_socket_t, const char*, int32_t);

static fn_send     g_send   = NULL;
static fn_recv     g_recv   = NULL;
static fn_free     g_free   = NULL;
static fn_strerror g_strerr = NULL;
static void*       g_nng    = NULL;

static bool loadNng()
{
    if (g_nng)
        return g_send && g_recv;
    g_nng = dlopen("libnng.so.1", RTLD_NOW);
    if (!g_nng)
        g_nng = dlopen("libnng.so", RTLD_NOW);
    if (!g_nng)
        return false;
    fn_req0_open req0_open = (fn_req0_open)dlsym(g_nng, "nng_req0_open");
    fn_dial      dial      = (fn_dial)     dlsym(g_nng, "nng_dial");
    fn_close     closef    = (fn_close)    dlsym(g_nng, "nng_close");
    fn_set_ms    set_ms    = (fn_set_ms)   dlsym(g_nng, "nng_socket_set_ms");
    g_send   = (fn_send)     dlsym(g_nng, "nng_send");
    g_recv   = (fn_recv)     dlsym(g_nng, "nng_recv");
    g_free   = (fn_free)     dlsym(g_nng, "nng_free");
    g_strerr = (fn_strerror) dlsym(g_nng, "nng_strerror");
    if (!req0_open || !dial || !g_send || !g_recv)
        return false;
    // Stash the rest in globals via the dial path in run(); see below.
    // (Kept simple: re-resolve there.)
    (void)req0_open; (void)dial; (void)closef; (void)set_ms;
    return true;
}

static std::string envStr(const char* key)
{
    const char* v = getenv(key);
    return (v && *v) ? std::string(v) : std::string();
}

// ---------------- small helpers ----------------
static std::string baseName(const std::string& p)
{
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? p : p.substr(i + 1);
}

static std::string dirName(const std::string& p)
{
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? std::string() : p.substr(0, i);
}

static std::string stemOf(const std::string& p)
{
    std::string b = baseName(p);
    size_t i = b.find_last_of('.');
    return i == std::string::npos ? b : b.substr(0, i);
}

static std::string lower(std::string s)
{
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = char(s[i] - 'A' + 'a');
    return s;
}

static std::string normSep(std::string s)
{
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == '\\') s[i] = '/';
    return s;
}

// Appends a code point as UTF-8, so paths with non-ASCII characters survive.
static void appendUtf8(std::string& out, unsigned cp)
{
    if (cp < 0x80) out.push_back(char(cp));
    else if (cp < 0x800)
    {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

static unsigned hex4(const std::string& s, size_t i)
{
    unsigned v = 0;
    for (size_t k = i; k < i + 4 && k < s.size(); ++k)
    {
        char c = s[k];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= unsigned(c - '0');
        else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
    }
    return v;
}

// Reads the JSON string starting at s[i] == '"'.  Leaves i past the close quote.
static std::string jsonString(const std::string& s, size_t& i)
{
    std::string out;
    ++i;                                   // opening quote
    while (i < s.size() && s[i] != '"')
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            char e = s[i + 1];
            i += 2;
            switch (e)
            {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u':
            {
                unsigned cp = hex4(s, i);
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size()
                        && s[i] == '\\' && s[i + 1] == 'u')
                {
                    unsigned lo = hex4(s, i + 2);
                    i += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                appendUtf8(out, cp);
                break;
            }
            default: out.push_back(e); break;   // covers \\ \" \/
            }
        }
        else
        {
            out.push_back(s[i]);
            ++i;
        }
    }
    ++i;                                   // closing quote
    return out;
}

static bool readFile(const std::string& path, std::string& out)
{
    FILE* fh = fopen(path.c_str(), "rb");
    if (!fh)
        return false;
    out.clear();
    char buf[65536];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), fh)) > 0)
        out.append(buf, n);
    fclose(fh);
    return true;
}

// Pulls the string entries of a named JSON array, wherever it appears.
static std::vector<std::string> jsonArrayOfStrings(const std::string& js, const std::string& key)
{
    std::vector<std::string> out;
    std::string needle = "\"" + key + "\"";
    size_t k = js.find(needle);
    if (k == std::string::npos)
        return out;
    size_t b = js.find('[', k);
    if (b == std::string::npos)
        return out;
    size_t i = b + 1;
    while (i < js.size())
    {
        if (js[i] == ']') break;
        if (js[i] == '"') out.push_back(jsonString(js, i));
        else ++i;
    }
    return out;
}

// ---------------- zip ----------------
static uint32_t crcTable[256];
static bool     crcReady = false;

static void crcInit()
{
    for (uint32_t i = 0; i < 256; ++i)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crcTable[i] = c;
    }
    crcReady = true;
}

static uint32_t crc32of(const std::string& data)
{
    if (!crcReady) crcInit();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < data.size(); ++i)
        c = crcTable[(c ^ uint8_t(data[i])) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void put16(std::string& o, uint16_t v)
{
    o.push_back(char(v & 0xFF));
    o.push_back(char((v >> 8) & 0xFF));
}

static void put32(std::string& o, uint32_t v)
{
    o.push_back(char(v & 0xFF));
    o.push_back(char((v >> 8) & 0xFF));
    o.push_back(char((v >> 16) & 0xFF));
    o.push_back(char((v >> 24) & 0xFF));
}

struct ZipEntry
{
    std::string fullPath;    // absolute UTF-8 path on disk
    std::string name;        // stored name, UTF-8 with '/' separators
};

// Raw deflate (windowBits -15) as a zip entry requires.  Returns false if the
// result would not be smaller, in which case the caller stores the entry.
static bool deflateBuffer(const std::string& in, std::string& out)
{
    if (in.empty())
        return false;
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;

    out.clear();
    out.resize(in.size() + (in.size() / 2) + 1024);

    zs.next_in   = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in  = (uInt)in.size();
    zs.next_out  = reinterpret_cast<Bytef*>(&out[0]);
    zs.avail_out = (uInt)out.size();

    int rc = deflate(&zs, Z_FINISH);
    size_t produced = out.size() - zs.avail_out;
    deflateEnd(&zs);

    if (rc != Z_STREAM_END || produced == 0 || produced >= in.size())
        return false;

    out.resize(produced);
    return true;
}

static std::string canonical(const std::string& p)
{
    try
    {
        return fs::weakly_canonical(fs::path(normSep(p))).string();
    }
    catch (...)
    {
        try { return fs::path(normSep(p)).lexically_normal().string(); }
        catch (...) { return normSep(p); }
    }
}

// Reads the quoted s-expression token that follows position i.  KiCad escapes
// with a backslash inside quoted strings.
static bool quotedAfter(const std::string& s, size_t i, std::string& out)
{
    while (i < s.size() && s[i] != '"' && s[i] != ')' && s[i] != '\n')
        ++i;
    if (i >= s.size() || s[i] != '"')
        return false;
    ++i;
    out.clear();
    while (i < s.size() && s[i] != '"')
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            out.push_back(s[i + 1]);
            i += 2;
        }
        else
        {
            out.push_back(s[i]);
            ++i;
        }
    }
    return true;
}

// Walks the sheet hierarchy from the root schematic, following every
// (property "Sheetfile" "<path>") reference.  Sheets may live in
// subdirectories, so references are resolved against the directory of the
// sheet that names them, then canonicalised.  Anything landing outside the
// project directory is reported rather than silently dropped.
static std::vector<ZipEntry> collectProjectFiles(const std::string& projDirUtf8,
                                                 const std::string& rootFileUtf8,
                                                 std::vector<std::string>& outside)
{
    std::vector<ZipEntry> out;
    std::string projDir = canonical(projDirUtf8);
    std::string projPrefix = lower(projDir) + "/";

    std::vector<std::string> queue;
    std::vector<std::string> seen;
    queue.push_back(canonical(projDir + "/" + normSep(rootFileUtf8)));

    auto alreadySeen = [&](const std::string& p)
    {
        std::string k = lower(p);
        for (size_t i = 0; i < seen.size(); ++i)
            if (seen[i] == k) return true;
        seen.push_back(k);
        return false;
    };

    auto entryNameFor = [&](const std::string& full) -> std::string
    {
        std::string lf = lower(full);
        std::string rel;
        if (lf.size() > projPrefix.size()
                && lf.compare(0, projPrefix.size(), projPrefix) == 0)
            rel = full.substr(projPrefix.size());
        else
        {
            outside.push_back(full);
            size_t i = full.find_last_of('/');
            rel = (i == std::string::npos) ? full : full.substr(i + 1);
        }
        return normSep(rel);
    };

    while (!queue.empty())
    {
        std::string cur = queue.back();
        queue.pop_back();
        if (alreadySeen(cur))
            continue;

        std::string content;
        if (!readFile(cur, content))
            continue;                          // missing sheet: reported by absence

        ZipEntry e;
        e.fullPath = cur;
        e.name     = entryNameFor(cur);
        out.push_back(e);

        std::string base = dirName(cur);
        size_t pos = 0;
        const std::string key = "\"Sheetfile\"";
        while ((pos = content.find(key, pos)) != std::string::npos)
        {
            std::string rel;
            if (quotedAfter(content, pos + key.size(), rel) && !rel.empty())
                queue.push_back(canonical(base + "/" + normSep(rel)));
            pos += key.size();
        }
    }

    // Project-level files, top level only: .kicad_pro and .kicad_dru
    try
    {
        for (const auto& de : fs::directory_iterator(fs::path(projDir)))
        {
            std::error_code ec;
            if (!de.is_regular_file(ec) || ec)
                continue;
            std::string nm = lower(de.path().filename().string());
            bool want = (nm.size() > 10 && nm.compare(nm.size() - 10, 10, ".kicad_pro") == 0)
                     || (nm.size() > 10 && nm.compare(nm.size() - 10, 10, ".kicad_dru") == 0);
            if (!want)
                continue;
            std::string full = canonical(de.path().string());
            if (alreadySeen(full))
                continue;
            ZipEntry e;
            e.fullPath = full;
            e.name     = de.path().filename().string();
            out.push_back(e);
        }
    }
    catch (...) {}

    return out;
}

// Minimal ZIP writer. Entry names are written as UTF-8 with the language-
// encoding flag (bit 11) set, so non-ASCII sheet names survive extraction.
static bool writeZip(const std::vector<ZipEntry>& files, const std::string& zipPath,
                     std::string& err, uint64_t& totalIn, uint64_t& totalOut)
{
    std::string out, central;
    uint32_t offset = 0;
    totalIn = 0;

    for (size_t i = 0; i < files.size(); ++i)
    {
        std::string data;
        if (!readFile(files[i].fullPath, data))
        {
            err = "could not read " + files[i].name;
            return false;
        }
        totalIn += data.size();

        uint32_t crc = crc32of(data);           // always of the UNCOMPRESSED bytes
        uint32_t sz  = uint32_t(data.size());
        const std::string& nm = files[i].name;

        std::string packed;
        uint16_t method = 0;
        if (deflateBuffer(data, packed))
            method = 8;
        else
            packed = data;
        uint32_t csz = uint32_t(packed.size());

        uint32_t localOffset = offset;

        std::string lh;
        put32(lh, 0x04034B50);
        put16(lh, 20);              // version needed
        put16(lh, 0x0800);          // flag: names are UTF-8
        put16(lh, method);          // 8 = deflate, 0 = store
        put16(lh, 0);               // mod time
        put16(lh, 0x21);            // mod date (1980-01-01)
        put32(lh, crc);
        put32(lh, csz);
        put32(lh, sz);
        put16(lh, uint16_t(nm.size()));
        put16(lh, 0);
        out += lh;
        out += nm;
        out += packed;
        offset += uint32_t(lh.size() + nm.size() + packed.size());

        std::string cd;
        put32(cd, 0x02014B50);
        put16(cd, 20);              // version made by
        put16(cd, 20);              // version needed
        put16(cd, 0x0800);
        put16(cd, method);
        put16(cd, 0);
        put16(cd, 0x21);
        put32(cd, crc);
        put32(cd, csz);
        put32(cd, sz);
        put16(cd, uint16_t(nm.size()));
        put16(cd, 0);               // extra
        put16(cd, 0);               // comment
        put16(cd, 0);               // disk
        put16(cd, 0);               // internal attrs
        put32(cd, 0);               // external attrs
        put32(cd, localOffset);
        central += cd;
        central += nm;
    }

    uint32_t centralOffset = offset;
    out += central;
    put32(out, 0x06054B50);
    put16(out, 0);
    put16(out, 0);
    put16(out, uint16_t(files.size()));
    put16(out, uint16_t(files.size()));
    put32(out, uint32_t(central.size()));
    put32(out, centralOffset);
    put16(out, 0);

    FILE* fh = fopen(zipPath.c_str(), "wb");
    if (!fh)
    {
        err = "could not create the archive";
        return false;
    }
    size_t written = fwrite(out.data(), 1, out.size(), fh);
    fclose(fh);
    if (written != out.size())
    {
        err = "short write to the archive";
        return false;
    }
    totalOut = out.size();
    return true;
}

// The KiCad data version ("10.0") taken from this executable's own location,
// e.g. ~/.local/share/kicad/10.0/3rdparty/plugins/.../tweb  -> "10.0".
static std::string kicadVersionFromOwnPath()
{
    std::string p = ownExePath();
    if (p.empty())
        return std::string();
    while (!p.empty())
    {
        p = dirName(p);
        if (p.empty()) break;
        std::string seg = baseName(p);
        bool digitsAndDot = !seg.empty();
        for (size_t i = 0; i < seg.size(); ++i)
            if (!((seg[i] >= '0' && seg[i] <= '9') || seg[i] == '.'))
                digitsAndDot = false;
        if (digitsAndDot && seg.find('.') != std::string::npos)
            return seg;
    }
    return std::string();
}

static std::string configBase()
{
    std::string xdg = envStr("XDG_CONFIG_HOME");
    if (!xdg.empty())
        return xdg;
    std::string home = envStr("HOME");
    if (home.empty())
        return std::string();
    return home + "/.config";
}

static std::string kicadSettingsPath(const std::string& ver)
{
    std::string base = configBase();
    if (base.empty())
        return std::string();
    if (!ver.empty())
        return base + "/kicad/" + ver + "/kicad.json";
    // Version undetermined (dev-run binary outside the plugins dir):
    // pick the highest installed kicad.json.
    std::string best;
    try
    {
        for (const auto& de : fs::directory_iterator(fs::path(base + "/kicad")))
        {
            std::error_code ec;
            if (!de.is_directory(ec) || ec)
                continue;
            std::string cand = de.path().string() + "/kicad.json";
            FILE* fh = fopen(cand.c_str(), "rb");
            if (!fh)
                continue;
            fclose(fh);
            if (cand > best)
                best = cand;
        }
    }
    catch (...) {}
    return best;
}

static std::string lastZipPath;     // set by packageProject, used for the hand-off

static bool packageProject(const std::string& projDir, const std::string& schFile,
                           const std::string& zipStem, std::ostringstream& rpt);
static void handOffToBrowser(const std::string& zipPath, std::ostringstream& rpt);
static int stageOnly(int argc, char** argv);
static int settingsOnly();
static std::string apiDisabledMessage();

// One request/reply cycle against KiCad's API.  Returns false only on transport
// failure; an AS_UNHANDLED reply is a successful call with status 5.
static bool apiCall(nng_socket_t s, const std::string& token, const std::string& typeName,
                    const std::string& cmdPayload, int& status, std::string& errMsg,
                    std::string& anyPayload, std::string& transportErr)
{
    std::string any;
    putBytes(any, 1, "type.googleapis.com/" + typeName);
    putBytes(any, 2, cmdPayload);
    std::string hdr;
    putBytes(hdr, 1, token);
    putBytes(hdr, 2, "tweb");
    std::string req;
    putBytes(req, 1, hdr);
    putBytes(req, 2, any);

    int rv = g_send(s, (void*)req.data(), req.size(), 0);
    if (rv) { transportErr = g_strerr ? g_strerr(rv) : "send failed"; return false; }

    char*  buf = NULL;
    size_t len = 0;
    rv = g_recv(s, &buf, &len, 1 /* NNG_FLAG_ALLOC */);
    if (rv) { transportErr = g_strerr ? g_strerr(rv) : "recv failed"; return false; }
    if (!buf || len == 0 || len > (16u << 20))
    {
        if (buf && g_free) g_free(buf, len);
        transportErr = "empty or implausible reply";
        return false;
    }
    std::string resp(buf, len);
    if (g_free) g_free(buf, len);

    status = -1;
    errMsg.clear();
    anyPayload.clear();

    Reader r(resp);
    int f, w; std::string v;
    while (r.next(f, w, v))
    {
        if (f == 2)
        {
            Reader sr(v); int sf, sw; std::string sv;
            while (sr.next(sf, sw, sv))
            {
                if (sf == 1 && !sv.empty()) status = (unsigned char)sv[0];
                else if (sf == 2)           errMsg = sv;
            }
        }
        else if (f == 3)
        {
            Reader ar(v); int af, aw; std::string av;
            while (ar.next(af, aw, av))
                if (af == 2) anyPayload = av;
        }
    }
    return true;
}

// Offline self-test: package a project without KiCad.
//   tweb --collect <project_dir> <root_sheet_filename>
static int collectOnly(int argc, char** argv)
{
    if (argc < 4)
    {
        logLine("--collect needs <project_dir> <root_sheet_filename>");
        return 1;
    }
    std::string dir  = argv[2];
    std::string root = argv[3];

    std::ostringstream rpt;
    rpt << "--collect " << dir << "  root=" << root << "\n\n";
    bool ok = packageProject(dir, root, stemOf(root), rpt);
    logLine((ok ? "COLLECT OK\n" : "COLLECT FAILED\n") + rpt.str() + "\n----");
    return ok ? 0 : 1;
}

static int run(int argc, char** argv)
{
    std::string a1 = (argc >= 2) ? argv[1] : std::string();
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--verbose")
            g_verbose = true;
    if (a1 == "--collect")
        return collectOnly(argc, argv);
    if (a1 == "--stage")
        return stageOnly(argc, argv);
    if (a1 == "--settings")
        return settingsOnly();
    // NOTE (Linux): Shift+click settings is unavailable - a plugin process
    // cannot query the key state. Use `tweb --settings` to change the address.

    std::ostringstream rpt;
    logLine("started");

    std::string sock  = envStr("KICAD_API_SOCKET");
    std::string token = envStr("KICAD_API_TOKEN");

    if (sock.empty())
    {
        // The one failure an ordinary user can hit, so it is a translated
        // string naming the exact menu path rather than a diagnostic.
        finish(apiDisabledMessage(), ICON_ERROR);
        return 1;
    }

    if (!loadNng())
    {
        finish("libnng.so.1 could not be loaded.", ICON_ERROR);
        return 1;
    }
    fn_req0_open req0_open = (fn_req0_open)dlsym(g_nng, "nng_req0_open");
    fn_dial      dial      = (fn_dial)     dlsym(g_nng, "nng_dial");
    fn_close     closef    = (fn_close)    dlsym(g_nng, "nng_close");
    fn_set_ms    set_ms    = (fn_set_ms)   dlsym(g_nng, "nng_socket_set_ms");
    if (!req0_open || !dial)
    {
        finish("libnng does not export the expected entry points.", ICON_ERROR);
        return 1;
    }

    nng_socket_t s = 0;
    int rv = req0_open(&s);
    if (rv) { finish("nng_req0_open failed.", ICON_ERROR); return 1; }
    if (set_ms) { set_ms(s, "recv-timeout", 5000); set_ms(s, "send-timeout", 5000); }

    rv = dial(s, sock.c_str(), NULL, 0);
    if (rv)
    {
        finish(std::string("nng_dial failed: ") + (g_strerr ? g_strerr(rv) : "error"),
               ICON_ERROR);
        return 1;
    }

    // ---- 1. which schematic is open, per KiCad itself ----
    int         status = -1;
    std::string errMsg, anyPayload, terr;
    std::string cmd;
    putEnum(cmd, 1, 1);                       // DOCTYPE_SCHEMATIC
    if (!apiCall(s, token, "kiapi.common.commands.GetOpenDocuments", cmd,
                 status, errMsg, anyPayload, terr))
    {
        finish("IPC failed: " + terr, ICON_ERROR);
        return 1;
    }

    // docSpec keeps the raw DocumentSpecifier so it can be echoed back verbatim
    // in later commands - KiCad identifies the document by that exact message.
    std::string schFile, docSpec;
    {
        Reader r(anyPayload);
        int f, w; std::string doc;
        while (r.next(f, w, doc))
        {
            if (f != 1) continue;
            if (docSpec.empty()) docSpec = doc;
            Reader d(doc); int df, dw; std::string dv;
            while (d.next(df, dw, dv))
                if (df == 4 && schFile.empty()) schFile = dv;
        }
    }

    // ---- save first, or the archive is of whatever was last written to
    //         disk rather than what is on screen ----
    if (!docSpec.empty())
    {
        std::string sd;
        putBytes(sd, 1, docSpec);
        int ss = -1;
        std::string se, sp, st2;
        if (apiCall(s, token, "kiapi.common.commands.SaveDocument", sd, ss, se, sp, st2))
        {
            rpt << "SaveDocument: status=" << ss
                << (ss == 1 ? "  (saved)"
                            : ss == 5 ? "  (AS_UNHANDLED - eeschema has no handler)" : "")
                << "\n";
            if (!se.empty())
                rpt << "  " << se << "\n";
            rpt << "\n";
        }
        else
        {
            rpt << "SaveDocument: transport error - " << st2 << "\n\n";
        }
    }

    if (closef) closef(s);

    rpt << "KiCad reports open schematic:\n  " << (schFile.empty() ? "(none)" : schFile) << "\n\n";
    if (status != 1 || schFile.empty())
    {
        rpt << "GetOpenDocuments status=" << status << "\n"
            << "No schematic is open, so there is nothing to submit.";
        finish(rpt.str(), ICON_ERROR);
        return 1;
    }

    // ---- 2. where projects live, per KiCad's settings ----
    std::string ver  = kicadVersionFromOwnPath();
    std::string cfg  = kicadSettingsPath(ver);
    rpt << "KiCad data version: " << (ver.empty() ? "(undetermined)" : ver) << "\n"
        << "settings: " << (cfg.empty() ? "(unresolved)" : cfg) << "\n\n";

    std::string js;
    if (cfg.empty() || !readFile(cfg, js))
    {
        rpt << "Could not read kicad.json - cannot resolve the project directory.";
        finish(rpt.str(), ICON_ERROR);
        return 1;
    }

    std::vector<std::string> open = jsonArrayOfStrings(js, "open_projects");
    rpt << "open_projects (" << open.size() << "):\n";
    for (size_t i = 0; i < open.size(); ++i)
        rpt << "  [" << i << "] " << open[i] << "\n";
    rpt << "\n";

    // ---- 3. match on the filename stem ----
    std::string want = lower(stemOf(schFile));
    std::string hit;
    for (size_t i = 0; i < open.size(); ++i)
    {
        if (lower(stemOf(open[i])) == want) { hit = open[i]; break; }
    }
    if (hit.empty() && open.size() == 1)
    {
        hit = open[0];                        // single project open: unambiguous
        rpt << "(stem did not match; only one project open, using it)\n\n";
    }

    if (hit.empty())
    {
        rpt << "Could not match '" << schFile << "' to an open project.\n"
            << "The user would be asked to pick the project here.";
        finish(rpt.str(), ICON_WARNING);
        return 1;
    }

    std::string projDir = dirName(hit);
    rpt << "RESOLVED\n"
        << "  .kicad_pro : " << hit << "\n"
        << "  project dir: " << projDir << "\n\n";

    // ---- 4. collect the submission set and build the archive ----
    if (!packageProject(projDir, schFile, stemOf(hit), rpt))
    {
        finish(rpt.str(), ICON_ERROR);
        return 1;
    }

    handOffToBrowser(lastZipPath, rpt);

    finish(rpt.str(), ICON_INFO);
    return 0;
}

// Collects the submission set and writes the archive.  Shared by the toolbar
// path and by --collect, so the packaging is tested by exactly the code that
// ships.
static bool packageProject(const std::string& projDir, const std::string& schFile,
                           const std::string& zipStem, std::ostringstream& rpt)
{
    std::vector<std::string> outside;
    std::vector<ZipEntry> files = collectProjectFiles(projDir, schFile, outside);
    rpt << "files to submit (" << files.size() << ", sheet hierarchy followed):\n";
    for (size_t i = 0; i < files.size() && i < 12; ++i)
        rpt << "  " << files[i].name << "\n";
    if (files.size() > 12)
        rpt << "  ... and " << (files.size() - 12) << " more\n";
    if (!outside.empty())
    {
        rpt << "\n  " << outside.size() << " sheet(s) live OUTSIDE the project dir:\n";
        for (size_t i = 0; i < outside.size() && i < 4; ++i)
            rpt << "    " << outside[i] << "\n";
    }
    rpt << "\n";

    if (files.empty())
    {
        rpt << "No schematic or project files found to submit.";
        return false;
    }

    std::string tmp = envStr("TMPDIR");
    if (tmp.empty())
        tmp = "/tmp";
    std::string outDir = tmp + "/tomachie";
    std::error_code ec;
    fs::create_directories(fs::path(outDir), ec);
    std::string zipPath = outDir + "/" + zipStem + ".zip";

    std::string err;
    uint64_t inBytes = 0, outBytes = 0;
    if (!writeZip(files, zipPath, err, inBytes, outBytes))
    {
        rpt << "zip failed: " << err;
        return false;
    }

    rpt << "ARCHIVE WRITTEN\n"
        << "  " << zipPath << "\n"
        << "  " << files.size() << " files, "
        << (inBytes / 1024) << " KB in, " << (outBytes / 1024) << " KB archive\n";

    lastZipPath = zipPath;
    return true;
}
// ---------------- host ----------------
//
// The one server this client talks to.  It is a plain constant: the source is
// public, so anyone can see where a design is sent.  tweb.json supplies only
// PATHS on this host.
static const char* TOMACHIE_HOST = "tomachie.com";

// tweb.json may only supply a path, never a whole URL, or a config file could
// send a user's design to another server.
static bool relativePathOk(const std::string& p)
{
    if (p.empty() || p[0] != '/')                   return false;   // must be rooted
    if (p.size() > 1 && p[1] == '/')                return false;   // //other.host
    if (p.find("://") != std::string::npos)         return false;   // scheme smuggled in
    if (p.find('\\') != std::string::npos)          return false;
    return true;
}

// Paths, not URLs.  tweb.json sits beside the binary:
//   { "stage_path": "/stage", "cda_path": "/{lang}/...", "lang": "" }
// A missing file or a path that is not relative is a hard stop with a clear
// message - a built-in default would go stale silently the day it moves.
struct Config
{
    std::string  host;         // TOMACHIE_HOST
    std::string  stagePath;
    std::string  cdaPath;
    std::string  learnPath;
    std::string  lang;         // one of the site's codes; empty = resolve from the OS
    bool         loaded = false;
    std::string  problem;
};

static std::string urlFor(const Config& c, const std::string& path)
{
    return "https://" + c.host + path;
}

static std::string jsonValueOf(const std::string& js, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    size_t k = js.find(needle);
    if (k == std::string::npos)
        return std::string();
    size_t q = js.find('"', k + needle.size());
    if (q == std::string::npos)
        return std::string();
    return jsonString(js, q);
}

static Config loadConfig()
{
    Config c;
    std::string dir = exeDir();
    if (dir.empty())
    {
        c.problem = "could not locate the tweb binary";
        return c;
    }
    std::string cfgPath = dir + "/tweb.json";

    std::string js;
    if (!readFile(cfgPath, js))
    {
        c.problem = "tweb.json not found beside tweb:\n  " + cfgPath;
        return c;
    }
    c.host = TOMACHIE_HOST;

    c.stagePath = jsonValueOf(js, "stage_path");
    c.cdaPath   = jsonValueOf(js, "cda_path");
    c.learnPath = jsonValueOf(js, "learn_path");
    c.lang      = jsonValueOf(js, "lang");

    if (!relativePathOk(c.stagePath))
    {
        c.problem = "tweb.json needs a relative \"stage_path\" such as \"/stage\":\n  " + cfgPath;
        return c;
    }
    if (!c.cdaPath.empty() && !relativePathOk(c.cdaPath))
    {
        c.problem = "tweb.json \"cda_path\" must be a relative path:\n  " + cfgPath;
        return c;
    }
    if (!c.learnPath.empty() && !relativePathOk(c.learnPath))
    {
        c.problem = "tweb.json \"learn_path\" must be a relative path:\n  " + cfgPath;
        return c;
    }

    c.loaded = true;
    return c;
}

// ---------------- language and strings ----------------
typedef StringTable Strings;

static bool loadStringsFile(const std::string& path, Strings& s)
{
    std::string text;
    if (!readFile(path, text))
        return false;
    return s.parse(text);
}

static std::string siteLanguage(const std::string& configured)
{
    return configured.empty() ? StringTable::osLanguageCode() : lower(configured);
}

// Loads i18n/tweb_<code>.txt beside the binary, falling back to English.
static Strings loadStrings(const std::string& preferred)
{
    std::string dir = exeDir();

    Strings s;
    std::string code = preferred.empty() ? StringTable::osLanguageCode() : lower(preferred);

    if (loadStringsFile(dir + "/i18n/tweb_" + code + ".txt", s))
    {
        s.code = code;
        return s;
    }
    Strings en;
    if (loadStringsFile(dir + "/i18n/tweb_en.txt", en))
        en.code = "en";
    return en;
}

static std::string formatOne(std::string text, const std::string& arg)
{
    size_t k = text.find("{0}");
    if (k != std::string::npos)
        text = text.substr(0, k) + arg + text.substr(k + 3);
    return text;
}

// cda_url in tweb.json carries {lang}, because the agreement is published per
// language (…/cn/…, …/kr/…) using Tomachie's codes, not ISO ones.
static std::string withLang(std::string url, const std::string& code)
{
    const std::string tag = "{lang}";
    size_t k = url.find(tag);
    while (k != std::string::npos)
    {
        url = url.substr(0, k) + code + url.substr(k + tag.size());
        k = url.find(tag);
    }
    return url;
}

// (Right-to-left layout is a Win32 dialog concern; the Linux zenity/console
// prompts need no equivalent.)

// Run a program with argv, optionally feeding stdin, capturing stdout.
// Returns the exit code; output holds stdout (stderr discarded unless
// includeStderr, in which case it is appended after stdout).
static int runCapture(const std::vector<std::string>& args, const std::string& stdinData,
                      std::string& output, bool includeStderr)
{
    int outPipe[2], inPipe[2];
    if (pipe(outPipe) != 0)
        return 127;
    bool needIn = !stdinData.empty();
    if (needIn && pipe(inPipe) != 0)
    {
        close(outPipe[0]); close(outPipe[1]);
        return 127;
    }
    pid_t pid = fork();
    if (pid < 0)
    {
        close(outPipe[0]); close(outPipe[1]);
        if (needIn) { close(inPipe[0]); close(inPipe[1]); }
        return 127;
    }
    if (pid == 0)
    {
        dup2(outPipe[1], STDOUT_FILENO);
        close(outPipe[0]); close(outPipe[1]);
        if (needIn)
        {
            dup2(inPipe[0], STDIN_FILENO);
            close(inPipe[0]); close(inPipe[1]);
        }
        else
        {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        }
        if (!includeStderr)
        {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        }
        std::vector<char*> av;
        for (size_t i = 0; i < args.size(); ++i)
            av.push_back(const_cast<char*>(args[i].c_str()));
        av.push_back(NULL);
        execvp(av[0], av.data());
        _exit(127);
    }
    close(outPipe[1]);
    if (needIn)
    {
        close(inPipe[0]);
        size_t off = 0;
        while (off < stdinData.size())
        {
            ssize_t n = write(inPipe[1], stdinData.data() + off, stdinData.size() - off);
            if (n <= 0) break;
            off += size_t(n);
        }
        close(inPipe[1]);
    }
    output.clear();
    char buf[65536];
    ssize_t n = 0;
    while ((n = read(outPipe[0], buf, sizeof(buf))) > 0)
        output.append(buf, size_t(n));
    close(outPipe[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return 128;
}

static void openUrl(const std::string& url)
{
    // Double fork: the grandchild is reparented to init, so the plugin
    // never has to reap the browser/file-manager and leaves no zombie.
    pid_t pid = fork();
    if (pid != 0)
    {
        if (pid > 0)
        {
            int st = 0;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        }
        return;   // parent carries on
    }
    if (fork() != 0)
        _exit(0);   // intermediate child: exit at once so the parent's wait ends
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0)
    {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > 2) close(devnull);
    }
    execlp("xdg-open", "xdg-open", url.c_str(), (char*)NULL);
    _exit(127);
}

// POST the archive to /stage as multipart/form-data and return the browser URL
// the server hands back.  Empty on any failure, with the reason in `why`.
// Uses the curl CLI so no TLS code ships in this binary.
static std::string stageArchive(const Config& cfg, const std::string& zipPath,
                                const std::string& zipName, const std::string& email,
                                const std::string& lang, std::string& why)
{
    std::string zip;
    if (!readFile(zipPath, zip) || zip.empty())
    {
        why = "archive could not be re-read";
        return std::string();
    }
    (void)zip; // sent by filename reference below; the re-read proves it exists

    std::string safeName = zipName;
    for (size_t i = 0; i < safeName.size(); ++i)
        if (safeName[i] == ';' || safeName[i] == '"' || safeName[i] == '\r' || safeName[i] == '\n')
            safeName[i] = '_';

    std::vector<std::string> args;
    args.push_back("curl");
    args.push_back("-sS");
    args.push_back("--connect-timeout");
    args.push_back("10");
    args.push_back("-m");
    args.push_back("150");
    args.push_back("-X");
    args.push_back("POST");
    // --form-string: with -F a value starting with '@' or '<' is read as a file.
    if (!email.empty())
    {
        args.push_back("--form-string");
        args.push_back("email=" + email);
    }
    if (!lang.empty())
    {
        args.push_back("--form-string");
        args.push_back("lang=" + lang);
    }
    args.push_back("-F");
    args.push_back("schematic=@" + zipPath + ";filename=" + safeName + ";type=application/zip");
    args.push_back("--write-out");
    args.push_back("\n%{http_code}");
    args.push_back(urlFor(cfg, cfg.stagePath));

    std::string out;
    int rc = runCapture(args, std::string(), out, true);
    if (rc != 0)
    {
        why = "upload failed (curl exit " + std::to_string(rc) + ")"
            + (out.empty() ? "" : ": " + out.substr(0, 200));
        return std::string();
    }
    // Split trailing HTTP code from the body.
    size_t nl = out.find_last_of('\n');
    std::string codeStr = (nl == std::string::npos) ? "" : out.substr(nl + 1);
    std::string reply = (nl == std::string::npos) ? out : out.substr(0, nl);
    // Strip stray \r.
    while (!codeStr.empty() && (codeStr.back() == '\r' || codeStr.back() == ' '))
        codeStr.pop_back();

    if (codeStr == "200")
    {
        std::string url = jsonValueOf(reply, "url");
        if (url.empty())
            why = "reply carried no url: " + reply.substr(0, 200);
        return url;
    }
    std::string e = jsonValueOf(reply, "error");
    why = "server returned HTTP " + (codeStr.empty() ? "?" : codeStr)
        + (e.empty() ? "" : " - " + e);
    return std::string();
}

static void copyToClipboard(const std::string& text)
{
    if (getenv("WAYLAND_DISPLAY") && haveProg("wl-copy"))
    {
        std::vector<std::string> args;
        args.push_back("wl-copy");
        std::string ignored;
        runCapture(args, text, ignored);
        return;
    }
    if (haveProg("xclip"))
    {
        std::vector<std::string> args;
        args.push_back("xclip");
        args.push_back("-selection");
        args.push_back("clipboard");
        std::string ignored;
        runCapture(args, text, ignored);
        return;
    }
    if (haveProg("xsel"))
    {
        std::vector<std::string> args;
        args.push_back("xsel");
        args.push_back("--clipboard");
        args.push_back("--input");
        std::string ignored;
        runCapture(args, text, ignored);
    }
}

// Offline self-test of the upload path: stage an existing archive and log the
// URL the server returns, without opening a browser.
//   tweb --stage <path_to_zip>
static int stageOnly(int argc, char** argv)
{
    if (argc < 3)
    {
        logLine("--stage needs <path_to_zip>");
        return 1;
    }
    std::string zip = argv[2];

    Config cfg = loadConfig();
    if (!cfg.loaded)
    {
        logLine("STAGE CONFIG ERROR: " + cfg.problem);
        return 1;
    }

    std::string name = baseName(zip);

    std::string why;
    std::string url = stageArchive(cfg, zip, name, std::string(), siteLanguage(cfg.lang), why);
    if (url.empty())
    {
        logLine("STAGE FAILED: " + why);
        return 1;
    }
    logLine("STAGE OK\n  " + name + "\n  " + url);
    return 0;
}

// ---------------- first-run settings ----------------
//
// Shown once.  It holds the email, states that the design leaves the computer,
// and links to the agreement on the website.  After it has been accepted the
// button just uploads; --settings reopens it to change the address.
static std::string settingsPath()
{
    // Inside KiCad's Flatpak, XDG_CONFIG_HOME is the sandbox's own folder
    // (~/.var/app/org.kicad.KiCad/config), which `tweb --settings` run from a
    // terminal never sees.  The sandbox can read the home folder, so the file
    // stays in ~/.config either way: one settings file per user.
    std::string base = configBase();
    if (!envStr("FLATPAK_ID").empty() && !envStr("HOME").empty())
        base = envStr("HOME") + "/.config";
    if (base.empty())
        return std::string();
    std::string dir = base + "/Tomachie";
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    return dir + "/tweb_user.json";
}

static std::string loadSavedEmail()
{
    std::string p = settingsPath(), js;
    if (p.empty() || !readFile(p, js))
        return std::string();
    return jsonValueOf(js, "email");
}

static void saveEmail(const std::string& email)
{
    std::string p = settingsPath();
    if (p.empty())
        return;
    // Minimal JSON escaping for the address field.
    std::string esc;
    for (size_t i = 0; i < email.size(); ++i)
    {
        char ch = email[i];
        if (ch == '"' || ch == '\\') esc.push_back('\\');
        esc.push_back(ch);
    }
    std::string js = "{\n  \"email\": \"" + esc + "\",\n  \"accepted\": true\n}\n";
    FILE* fh = fopen(p.c_str(), "w");
    if (!fh)
        return;
    fwrite(js.data(), 1, js.size(), fh);
    fclose(fh);
    chmod(p.c_str(), 0600);
}

// Returns true if the user accepted; email carries the address.
static bool showSettings(const Strings& s, const std::string& cdaUrl,
                         const std::string& learnUrl,
                         const std::string& projectName, std::string& email)
{
    std::string title = formatOne(s.get("title"), projectName);
    std::string body = s.get("heading") + "\n\n"
        + s.get("intro") + "\n\n"
        + formatOne(s.get("offsite"), TOMACHIE_HOST) + "\n\n"
        + s.get("save_first") + "\n\n"
        + s.get("cda_link") + ":\n" + cdaUrl + "\n\n"
        + s.get("learn_link") + ":\n" + learnUrl + "\n\n"
        + s.get("settings_hint")
        + "\n(On Linux, instead of Shift+click, run in a terminal:\n  "
        + ownExePath() + " --settings )";

    const std::vector<std::string>& dialog = dialogCommand();
    if (!dialog.empty())
    {
        std::vector<std::string> args = dialog;
        args.push_back("--forms");
        args.push_back("--title=" + title);
        args.push_back("--text=" + body);
        args.push_back("--add-entry=" + s.get("email"));
        args.push_back("--add-entry=" + s.get("confirm_email"));
        args.push_back("--ok-label=" + s.get("send"));
        args.push_back("--cancel-label=" + s.get("cancel"));
        std::string out;
        int rc = runCapture(args, std::string(), out);
        if (rc != 0)
            return false;   // cancelled
        // zenity separates fields with '|'.
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        size_t bar = out.find('|');
        std::string a = (bar == std::string::npos) ? out : out.substr(0, bar);
        std::string b = (bar == std::string::npos) ? std::string() : out.substr(bar + 1);
        if (a != b)
        {
            std::vector<std::string> w = dialog;
            w.push_back("--warning");
            w.push_back("--title=Tomachie");
            w.push_back("--text=" + s.get("email_mismatch"));
            std::string ignored;
            runCapture(w, std::string(), ignored);
            return false;
        }
        if (a.empty() || a.find('@') == std::string::npos)
            return false;   // nothing useful entered
        email = a;
        return true;
    }

    if (isatty(STDIN_FILENO))
    {
        printf("\n%s\n\n%s\n\n", title.c_str(), body.c_str());
        if (!email.empty())
            printf("Current address: %s\n", email.c_str());
        char a[512] = {0}, b[512] = {0};
        printf("%s: ", s.get("email").c_str());
        fflush(stdout);
        if (!fgets(a, sizeof(a), stdin))
            return false;
        printf("%s: ", s.get("confirm_email").c_str());
        fflush(stdout);
        if (!fgets(b, sizeof(b), stdin))
            return false;
        std::string sa = a, sb = b;
        while (!sa.empty() && (sa.back() == '\n' || sa.back() == '\r')) sa.pop_back();
        while (!sb.empty() && (sb.back() == '\n' || sb.back() == '\r')) sb.pop_back();
        if (sa != sb)
        {
            printf("%s\n", s.get("email_mismatch").c_str());
            return false;
        }
        if (sa.empty() || sa.find('@') == std::string::npos)
            return false;
        email = sa;
        return true;
    }

    // Launched by KiCad with no terminal and no GUI helper: point at --settings.
    finish("No email is stored yet. Run `tweb --settings` in a terminal once to set it.",
           ICON_ERROR);
    return false;
}

// The API-disabled message, localized.
static std::string apiDisabledMessage()
{
    Config c = loadConfig();
    Strings s = loadStrings(c.loaded ? c.lang : std::string());
    return s.get("api_disabled");
}

// Reopen the settings dialog to change the stored address.
//   tweb --settings
static int settingsOnly()
{
    Config cfg = loadConfig();
    Strings s  = loadStrings(cfg.loaded ? cfg.lang : std::string());
    std::string email = loadSavedEmail();          // prefill with what is in use
    std::string cda = cfg.loaded ? withLang(urlFor(cfg, cfg.cdaPath), siteLanguage(cfg.lang)) : "";
    std::string learn = cfg.loaded ? withLang(urlFor(cfg, cfg.learnPath), siteLanguage(cfg.lang)) : "";
    if (!showSettings(s, cda, learn, "settings", email))
    {
        logLine("SETTINGS cancelled");
        return 1;
    }
    saveEmail(email);
    logLine("SETTINGS saved: " + email + "  (language " + s.code + ")");
    return 0;
}

static void revealInExplorer(const std::string& zipPath)
{
    copyToClipboard(zipPath);
    openUrl(dirName(zipPath));
}

// Stage the archive, then open the URL the server returns.  The page fills
// itself in from the token; the person picks the options and presses Analyze,
// which is the only thing that creates a job.
static void handOffToBrowser(const std::string& zipPath, std::ostringstream& rpt)
{
    Config cfg = loadConfig();
    if (!cfg.loaded)
    {
        rpt << "\nCONFIGURATION MISSING\n  " << cfg.problem << "\n"
            << "\nThe archive is in the file manager and its path is on the\n"
               "clipboard, so it can be uploaded by hand.\n";
        revealInExplorer(zipPath);
        return;
    }

    std::string zipName = baseName(zipPath);

    Strings     s    = loadStrings(cfg.lang);        // dialog text (may be the EN fallback)
    std::string site = siteLanguage(cfg.lang);       // website language (never falls back)

    // First run only: email, the off-site notice and the agreement link.
    // Once accepted, the button just uploads.
    std::string email = loadSavedEmail();
    if (email.empty())
    {
        if (!showSettings(s, withLang(urlFor(cfg, cfg.cdaPath), site),
                          withLang(urlFor(cfg, cfg.learnPath), site),
                          stemOf(zipName), email))
        {
            rpt << "\nCancelled - nothing was sent.\n";
            return;
        }
        saveEmail(email);
    }

    std::string why;
    std::string url = stageArchive(cfg, zipPath, zipName, email, site, why);
    rpt << "language: site=" << site << "  dialog=" << s.code << "\n";

    if (!url.empty())
    {
        rpt << "\nSTAGED\n  " << url << "\n\n"
            << "Opening the browser.  Choose the Design-for-Test options and\n"
               "press Analyze there - that is what starts the job.\n";
        openUrl(url);
    }
    else
    {
        rpt << "\nUPLOAD FAILED\n  " << why << "\n\n"
            << "The archive is in the file manager and its path is on the\n"
               "clipboard, so it can still be uploaded by hand.\n";
        revealInExplorer(zipPath);
    }
}

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);   // curl-pipe writes must fail, not kill us
    // NOTE: SIGCHLD stays at its default so waitpid() below reaps the
    // curl/zenity helpers. (An earlier build ignored SIGCHLD, which made
    // waitpid fail instantly and spun forever - every click hung.)
    try
    {
        return run(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::string msg = std::string("tweb failed: ") + e.what();
        logLine(msg);
        finish(msg, ICON_ERROR);
        return 2;
    }
    catch (...)
    {
        logLine("tweb failed with an unknown exception");
        finish("tweb failed with an unknown exception", ICON_ERROR);
        return 2;
    }
}
