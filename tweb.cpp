// tweb.exe - Tomachie web client, KiCad side.
//
// Resolves the project directory of the schematic open in KiCad, which is what
// the zip-and-submit step needs.  KiCad hands a plugin nothing about the design
// except an IPC socket and token.
//
// Two sources are combined, because neither is sufficient alone:
//
//   1. IPC GetOpenDocuments(DOCTYPE_SCHEMATIC) returns the open schematic's
//      FILENAME (no directory).  On KiCad 10.0 its ProjectSpecifier field is
//      absent entirely - verified from the raw bytes - so the path is not
//      available over IPC.  ExpandTextVariables (${KIPRJMOD}) and
//      GetOpenDocuments(DOCTYPE_UNKNOWN) both answer AS_UNHANDLED in eeschema.
//
//   2. kicad.json -> /system/open_projects[] holds full .kicad_pro paths, and
//      is written while KiCad runs.  But it is shared by every KiCad instance.
//
// Matching the IPC filename's stem against the open_projects entries picks the
// right project even with several instances running, since the IPC socket is
// per-instance.  IPC is the authority on WHAT is open; the settings file only
// supplies WHERE.
//
// Wire format taken from descriptors compiled into KiCad 10.0 kiapi.dll, so no
// protobuf library is needed.  Only dependency is nng.dll, which KiCad ships.

#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <cstdint>
#include <sstream>

#include "string_table.h"
#include "version.h"

#define TWEB_WIDEN2(s) L##s
#define TWEB_WIDEN(s)  TWEB_WIDEN2(s)
#define TWEB_VERSION_W TWEB_WIDEN(TWEB_VERSION)

static const char* LOG_NAME = "tweb_log.txt";

static void logLine(const std::string& text)
{
    HANDLE fh = CreateFileA(LOG_NAME, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[32];
    wsprintfA(stamp, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    std::string line = std::string(stamp) + text + "\r\n";
    DWORD written = 0;
    WriteFile(fh, line.data(), DWORD(line.size()), &written, NULL);
    FlushFileBuffers(fh);
    CloseHandle(fh);
}

static bool g_verbose = false;      // --verbose shows the success report

// Always logged.  Errors are always shown; the success report is diagnostic
// noise once the browser opens, so it is shown only with --verbose.
static void finish(const std::string& text, UINT icon)
{
    logLine("RESULT:\r\n" + text + "\r\n----");
    if (icon != MB_ICONINFORMATION || g_verbose)
        MessageBoxA(NULL, text.c_str(), "Tomachie", icon);
}

// ---------------- protobuf ----------------
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

// ---------------- nng ----------------
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

static std::string env(const char* key)
{
    char buf[8192];
    DWORD n = GetEnvironmentVariableA(key, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return std::string();
    return std::string(buf, n);
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
    HANDLE fh = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE)
        return false;
    out.clear();
    char buf[8192];
    DWORD n = 0;
    while (ReadFile(fh, buf, sizeof(buf), &n, NULL) && n > 0)
        out.append(buf, n);
    CloseHandle(fh);
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

// ---------------- Unicode paths ----------------
// Project paths come out of kicad.json as UTF-8 and may contain CJK or Cyrillic
// characters.  Every filesystem call below goes through the WIDE API; a narrow
// call would decode the bytes with the ANSI code page and miss the file.
static std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), NULL, 0);
    if (n <= 0)
        return std::wstring();
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

static std::string wideToUtf8(const std::wstring& w)
{
    if (w.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), NULL, 0, NULL, NULL);
    if (n <= 0)
        return std::string();
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, NULL, NULL);
    return s;
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

static bool readFileW(const std::wstring& path, std::string& out)
{
    HANDLE fh = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE)
        return false;
    out.clear();
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(fh, buf, sizeof(buf), &n, NULL) && n > 0)
        out.append(buf, n);
    CloseHandle(fh);
    return true;
}

struct ZipEntry
{
    std::wstring fullPath;
    std::string  name;        // stored name, UTF-8
};

// ---------------- deflate, via the zlib KiCad already ships ----------------
// Layout must match zlib's z_stream.  On MSVC x64 unsigned long is 4 bytes and
// pointers are 8, giving sizeof == 88, which is what zlib1.dll expects; the
// value is passed as stream_size so a mismatch is rejected rather than
// corrupting memory.
struct ZStream
{
    const unsigned char* next_in;
    unsigned int         avail_in;
    unsigned long        total_in;
    unsigned char*       next_out;
    unsigned int         avail_out;
    unsigned long        total_out;
    const char*          msg;
    void*                state;
    void*                zalloc;
    void*                zfree;
    void*                opaque;
    int                  data_type;
    unsigned long        adler;
    unsigned long        reserved;
};

typedef int (*fn_deflateInit2)(ZStream*, int, int, int, int, int, const char*, int);
typedef int (*fn_deflate)(ZStream*, int);
typedef int (*fn_deflateEnd)(ZStream*);

static fn_deflateInit2 z_init = NULL;
static fn_deflate      z_def  = NULL;
static fn_deflateEnd   z_end  = NULL;
static bool            zTried = false;

static void loadZlib()
{
    if (zTried) return;
    zTried = true;
    HMODULE z = LoadLibraryA("zlib1.dll");
    if (!z) z = LoadLibraryA("C:\\Program Files\\KiCad\\10.0\\bin\\zlib1.dll");
    if (!z) return;
    z_init = (fn_deflateInit2)GetProcAddress(z, "deflateInit2_");
    z_def  = (fn_deflate)     GetProcAddress(z, "deflate");
    z_end  = (fn_deflateEnd)  GetProcAddress(z, "deflateEnd");
}

// Raw deflate (windowBits -15) as a zip entry requires.  Returns false if zlib
// is unavailable or the result would not be smaller, in which case the caller
// stores the entry instead.
static bool deflateBuffer(const std::string& in, std::string& out)
{
    loadZlib();
    if (!z_init || !z_def || !z_end || in.empty())
        return false;

    ZStream zs;
    ZeroMemory(&zs, sizeof(zs));
    if (z_init(&zs, 6, 8, -15, 8, 0, "1.2.11", int(sizeof(ZStream))) != 0)
        return false;

    out.clear();
    out.resize(in.size() + (in.size() / 2) + 1024);

    zs.next_in   = reinterpret_cast<const unsigned char*>(in.data());
    zs.avail_in  = (unsigned int)in.size();
    zs.next_out  = reinterpret_cast<unsigned char*>(&out[0]);
    zs.avail_out = (unsigned int)out.size();

    int rc = z_def(&zs, 4 /* Z_FINISH */);
    size_t produced = out.size() - zs.avail_out;
    z_end(&zs);

    if (rc != 1 /* Z_STREAM_END */ || produced == 0 || produced >= in.size())
        return false;

    out.resize(produced);
    return true;
}

// The file set Tomachie expects: schematic + project files only.  Explicitly
// NOT a whole-directory sweep - a project folder also holds .history,
// _restore_backup_*, output/, backups and previous run artifacts, which are
// both large and the customer's earlier revisions.
static bool wantedExtension(const std::wstring& name)
{
    static const wchar_t* exts[] = { L".kicad_sch", L".kicad_pro", L".kicad_dru" };
    std::wstring lower;
    for (size_t i = 0; i < name.size(); ++i)
        lower.push_back(wchar_t(towlower(name[i])));
    for (size_t e = 0; e < 3; ++e)
    {
        std::wstring ext = exts[e];
        if (lower.size() >= ext.size()
                && lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0)
            return true;
    }
    return false;
}

static std::wstring lowerW(std::wstring s)
{
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = wchar_t(towlower(s[i]));
    return s;
}

static std::wstring canonical(const std::wstring& p)
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 4, buf, NULL);
    if (n == 0 || n >= MAX_PATH * 4)
        return p;
    return std::wstring(buf, n);
}

static std::wstring dirNameW(const std::wstring& p)
{
    size_t i = p.find_last_of(L"/\\");
    return i == std::wstring::npos ? std::wstring() : p.substr(0, i);
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
    std::wstring projDir = canonical(utf8ToWide(projDirUtf8));
    std::wstring projPrefix = lowerW(projDir) + L"\\";

    std::vector<std::wstring> queue;
    std::vector<std::wstring> seen;
    queue.push_back(canonical(projDir + L"\\" + utf8ToWide(rootFileUtf8)));

    auto alreadySeen = [&](const std::wstring& p)
    {
        std::wstring k = lowerW(p);
        for (size_t i = 0; i < seen.size(); ++i)
            if (seen[i] == k) return true;
        seen.push_back(k);
        return false;
    };

    auto entryNameFor = [&](const std::wstring& full) -> std::string
    {
        std::wstring lw = lowerW(full);
        std::wstring rel;
        if (lw.size() > projPrefix.size()
                && lw.compare(0, projPrefix.size(), projPrefix) == 0)
            rel = full.substr(projPrefix.size());
        else
        {
            outside.push_back(wideToUtf8(full));
            size_t i = full.find_last_of(L"/\\");
            rel = (i == std::wstring::npos) ? full : full.substr(i + 1);
        }
        for (size_t i = 0; i < rel.size(); ++i)
            if (rel[i] == L'\\') rel[i] = L'/';   // zip entries use '/'
        return wideToUtf8(rel);
    };

    while (!queue.empty())
    {
        std::wstring cur = queue.back();
        queue.pop_back();
        if (alreadySeen(cur))
            continue;

        std::string content;
        if (!readFileW(cur, content))
            continue;                          // missing sheet: reported by absence

        ZipEntry e;
        e.fullPath = cur;
        e.name     = entryNameFor(cur);
        out.push_back(e);

        std::wstring base = dirNameW(cur);
        size_t pos = 0;
        const std::string key = "\"Sheetfile\"";
        while ((pos = content.find(key, pos)) != std::string::npos)
        {
            std::string rel;
            if (quotedAfter(content, pos + key.size(), rel) && !rel.empty())
            {
                std::wstring w = utf8ToWide(rel);
                for (size_t i = 0; i < w.size(); ++i)
                    if (w[i] == L'/') w[i] = L'\\';
                queue.push_back(canonical(base + L"\\" + w));
            }
            pos += key.size();
        }
    }

    // Project-level files, top level only: .kicad_pro and .kicad_dru
    WIN32_FIND_DATAW fd;
    std::wstring pattern = projDir + L"\\*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            std::wstring nm = lowerW(fd.cFileName);
            bool want = (nm.size() > 10 && nm.compare(nm.size() - 10, 10, L".kicad_pro") == 0)
                     || (nm.size() > 10 && nm.compare(nm.size() - 10, 10, L".kicad_dru") == 0);
            if (!want)
                continue;
            std::wstring full = projDir + L"\\" + fd.cFileName;
            if (alreadySeen(full))
                continue;
            ZipEntry e;
            e.fullPath = full;
            e.name     = wideToUtf8(std::wstring(fd.cFileName));
            out.push_back(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    return out;
}

// Minimal ZIP writer, STORE method.  Deliberately does not shell out to
// Compress-Archive.  Entry names are written as UTF-8 with the language-
// encoding flag (bit 11) set, so non-ASCII sheet names survive extraction.
static bool writeZip(const std::vector<ZipEntry>& files, const std::wstring& zipPath,
                     std::string& err, uint64_t& totalIn, uint64_t& totalOut)
{
    std::string out, central;
    uint32_t offset = 0;
    totalIn = 0;

    for (size_t i = 0; i < files.size(); ++i)
    {
        std::string data;
        if (!readFileW(files[i].fullPath, data))
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

    HANDLE fh = CreateFileW(zipPath.c_str(), GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE)
    {
        err = "could not create the archive";
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(fh, out.data(), DWORD(out.size()), &written, NULL);
    CloseHandle(fh);
    if (!ok || written != out.size())
    {
        err = "short write to the archive";
        return false;
    }
    totalOut = out.size();
    return true;
}

// The KiCad data version ("10.0") taken from this executable's own location,
// e.g. ...\Documents\KiCad\10.0\plugins\tomachie\tweb.exe  -> "10.0".
// Works for PCM installs too (...\10.0\3rdparty\plugins\...).
static std::string kicadVersionFromOwnPath()
{
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (n == 0)
        return std::string();
    std::string p(buf, n);
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

static std::string kicadSettingsPath(const std::string& ver)
{
    std::string appdata = env("APPDATA");
    if (appdata.empty() || ver.empty())
        return std::string();
    return appdata + "\\kicad\\" + ver + "\\kicad.json";
}

static std::wstring lastZipPath;     // set by packageProject, used for the hand-off

static bool packageProject(const std::string& projDir, const std::string& schFile,
                           const std::string& zipStem, std::ostringstream& rpt);
static void handOffToBrowser(const std::wstring& zipPath, std::ostringstream& rpt);
static int stageOnly();      // defined below, after Config
static int settingsOnly();   // defined below, after the dialog
static std::string apiDisabledMessage();   // defined below, after Config/Strings

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
//   tweb.exe --collect <project_dir> <root_sheet_filename>
// Writes the same report to tweb_log.txt that the toolbar path produces.
static int collectOnly()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 4)
    {
        logLine("--collect needs <project_dir> <root_sheet_filename>");
        return 1;
    }
    std::string dir  = wideToUtf8(argv[2]);
    std::string root = wideToUtf8(argv[3]);
    LocalFree(argv);

    std::ostringstream rpt;
    rpt << "--collect " << dir << "  root=" << root << "\n\n";
    bool ok = packageProject(dir, root, stemOf(root), rpt);
    logLine((ok ? "COLLECT OK\r\n" : "COLLECT FAILED\r\n") + rpt.str() + "\r\n----");
    return ok ? 0 : 1;
}

static int run()
{
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring a1 = (argv && argc >= 2) ? std::wstring(argv[1]) : std::wstring();
        for (int i = 1; argv && i < argc; ++i)
            if (std::wstring(argv[i]) == L"--verbose")
                g_verbose = true;
        if (argv) LocalFree(argv);
        if (a1 == L"--collect")
            return collectOnly();
        if (a1 == L"--stage")
            return stageOnly();
        if (a1 == L"--settings")
            return settingsOnly();

        // KiCad gives a plugin one button and one left-click - the manifest has
        // no context-menu or secondary-action field, so right-click is not
        // available to us.  The process starts while the key is still down,
        // so Shift+click on the same icon opens the settings instead.
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        {
            logLine("shift held at launch - opening settings");
            return settingsOnly();
        }
    }

    std::ostringstream rpt;
    logLine("started");

    std::string sock  = env("KICAD_API_SOCKET");
    std::string token = env("KICAD_API_TOKEN");

    if (sock.empty())
    {
        // The one failure an ordinary user can hit, so it is a translated
        // string naming the exact menu path rather than a diagnostic.
        finish(apiDisabledMessage(), MB_ICONERROR);
        return 1;
    }

    HMODULE h = LoadLibraryA("nng.dll");
    if (!h) h = LoadLibraryA("C:\\Program Files\\KiCad\\10.0\\bin\\nng.dll");
    if (!h) { finish("nng.dll could not be loaded.", MB_ICONERROR); return 1; }

    fn_req0_open req0_open = (fn_req0_open)GetProcAddress(h, "nng_req0_open");
    fn_dial      dial      = (fn_dial)     GetProcAddress(h, "nng_dial");
    fn_close     closef    = (fn_close)    GetProcAddress(h, "nng_close");
    fn_set_ms    set_ms    = (fn_set_ms)   GetProcAddress(h, "nng_socket_set_ms");
    g_send   = (fn_send)     GetProcAddress(h, "nng_send");
    g_recv   = (fn_recv)     GetProcAddress(h, "nng_recv");
    g_free   = (fn_free)     GetProcAddress(h, "nng_free");
    g_strerr = (fn_strerror) GetProcAddress(h, "nng_strerror");

    if (!req0_open || !dial || !g_send || !g_recv)
    {
        finish("nng.dll does not export the expected entry points.", MB_ICONERROR);
        return 1;
    }

    nng_socket_t s = 0;
    int rv = req0_open(&s);
    if (rv) { finish("nng_req0_open failed.", MB_ICONERROR); return 1; }
    if (set_ms) { set_ms(s, "recv-timeout", 5000); set_ms(s, "send-timeout", 5000); }

    rv = dial(s, sock.c_str(), NULL, 0);
    if (rv)
    {
        finish(std::string("nng_dial failed: ") + (g_strerr ? g_strerr(rv) : "error"),
               MB_ICONERROR);
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
        finish("IPC failed: " + terr, MB_ICONERROR);
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

    // ---- 2. save first, or the archive is of whatever was last written to
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
        finish(rpt.str(), MB_ICONERROR);
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
        finish(rpt.str(), MB_ICONERROR);
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
        finish(rpt.str(), MB_ICONEXCLAMATION);
        return 1;
    }

    std::string projDir = dirName(hit);
    rpt << "RESOLVED\n"
        << "  .kicad_pro : " << hit << "\n"
        << "  project dir: " << projDir << "\n\n";

    // ---- 4. collect the submission set and build the archive ----
    if (!packageProject(projDir, schFile, stemOf(hit), rpt))
    {
        finish(rpt.str(), MB_ICONERROR);
        return 1;
    }

    handOffToBrowser(lastZipPath, rpt);

    finish(rpt.str(), MB_ICONINFORMATION);
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

    std::string tmp = env("TEMP");
    if (tmp.empty())
        tmp = env("TMP");
    std::wstring outDir = utf8ToWide(tmp) + L"\\tomachie";
    CreateDirectoryW(outDir.c_str(), NULL);
    std::wstring zipPath = outDir + L"\\" + utf8ToWide(zipStem) + L".zip";

    std::string err;
    uint64_t inBytes = 0, outBytes = 0;
    if (!writeZip(files, zipPath, err, inBytes, outBytes))
    {
        rpt << "zip failed: " << err;
        return false;
    }

    rpt << "ARCHIVE WRITTEN\n"
        << "  " << wideToUtf8(zipPath) << "\n"
        << "  " << files.size() << " files, "
        << (inBytes / 1024) << " KB in, " << (outBytes / 1024) << " KB archive\n";

    lastZipPath = zipPath;
    return true;
}

// ---------------- hand-off to the website ----------------
//
// Preferred path: POST the archive, then open the submit page with the returned
// id, so the page shows the file already received.  A browser cannot be made to
// fill a file input - that is forbidden - so this is the only way to reach that
// state, and it needs the server to hold the bytes.
//
// Fallback, used whenever the upload does not succeed (including before the
// endpoint exists): put the archive where the user can grab it in one action -
// selected in Explorer, path on the clipboard - and open the plain page.
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

// Paths, not URLs.  tweb.json sits beside the exe:
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
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (n == 0)
    {
        c.problem = "could not locate tweb.exe";
        return c;
    }
    std::string cfgPath = dirName(std::string(buf, n)) + "\\tweb.json";

    std::string js;
    if (!readFile(cfgPath, js))
    {
        c.problem = "tweb.json not found beside tweb.exe:\n  " + cfgPath;
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
//
// Tomachie's codes are mostly ISO 639-1 already (ar de en es fr he it ja pl pt
// ru th tr ur), so the OS code is used as the filename directly; the mapping
// lives in StringTable, shared with the setup program.
static std::string osLanguageCode()
{
    return StringTable::osLanguageCode();
}

static bool isRightToLeft(const std::string& code)
{
    return code == "ar" || code == "he" || code == "ur";
}

typedef StringTable Strings;

static bool loadStringsFile(const std::string& path, Strings& s)
{
    std::string text;
    if (!readFile(path, text))
        return false;
    return s.parse(text);
}

// The language the WEBSITE should use.  Deliberately independent of whether a
// translated dialog file exists: the site publishes all 18 languages, so a
// German user must get the German page even while the dialog falls back to
// English.  Tying the two together silently sent lang=en for every language we
// had not translated yet.
static std::string siteLanguage(const std::string& configured)
{
    return configured.empty() ? osLanguageCode() : lower(configured);
}

// Loads i18n/tweb_<code>.txt beside the exe, falling back to English.
static Strings loadStrings(const std::string& preferred)
{
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string dir = (n == 0) ? std::string() : dirName(std::string(buf, n));

    Strings s;
    std::string code = preferred.empty() ? osLanguageCode() : lower(preferred);

    if (loadStringsFile(dir + "\\i18n\\tweb_" + code + ".txt", s))
    {
        s.code = code;
        return s;
    }
    Strings en;
    if (loadStringsFile(dir + "\\i18n\\tweb_en.txt", en))
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

static std::string makeBoundary()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    char b[64];
    wsprintfA(b, "----tweb%08X%08X", (unsigned)GetCurrentProcessId(), (unsigned)t.LowPart);
    return std::string(b);
}

// POST the archive to /stage as multipart/form-data and return the browser URL
// the server hands back.  Empty on any failure, with the reason in `why`.
static std::string stageArchive(const Config& cfg, const std::wstring& zipPath,
                                const std::string& zipName, const std::string& email,
                                const std::string& lang, std::string& why)
{
    std::string zip;
    if (!readFileW(zipPath, zip) || zip.empty())
    {
        why = "archive could not be re-read";
        return std::string();
    }

    std::string bnd = makeBoundary();
    std::string body;
    if (!email.empty())
    {
        body += "--" + bnd + "\r\n";
        body += "Content-Disposition: form-data; name=\"email\"\r\n\r\n";
        body += email + "\r\n";
    }
    // The RESOLVED language, so the page opens in the same language as the
    // dialog.  Omitted only if resolution failed, in which case the server
    // defaults to English.
    if (!lang.empty())
    {
        body += "--" + bnd + "\r\n";
        body += "Content-Disposition: form-data; name=\"lang\"\r\n\r\n";
        body += lang + "\r\n";
    }
    body += "--" + bnd + "\r\n";
    body += "Content-Disposition: form-data; name=\"schematic\"; filename=\"" + zipName + "\"\r\n";
    body += "Content-Type: application/zip\r\n\r\n";
    body += zip;
    body += "\r\n--" + bnd + "--\r\n";

    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize     = sizeof(uc);
    wchar_t host[256]   = {0};
    wchar_t path[1024]  = {0};
    uc.lpszHostName     = host;  uc.dwHostNameLength     = 255;
    uc.lpszUrlPath      = path;  uc.dwUrlPathLength      = 1023;
    if (!WinHttpCrackUrl(utf8ToWide(urlFor(cfg, cfg.stagePath)).c_str(), 0, 0, &uc))
    {
        why = "the stage endpoint is not a valid URL";
        return std::string();
    }
    bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET ses = WinHttpOpen(L"tweb/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { why = "WinHttpOpen failed"; return std::string(); }
    WinHttpSetTimeouts(ses, 10000, 10000, 30000, 120000);

    std::string url;
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (con)
    {
        HINTERNET req = WinHttpOpenRequest(con, L"POST", path, NULL, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           secure ? WINHTTP_FLAG_SECURE : 0);
        if (req)
        {
            std::wstring hdrs = L"Content-Type: multipart/form-data; boundary="
                              + utf8ToWide(bnd) + L"\r\n";
            if (WinHttpSendRequest(req, hdrs.c_str(), DWORD(-1), NULL, 0,
                                   DWORD(body.size()), 0)
                    && WinHttpWriteData(req, body.data(), DWORD(body.size()), NULL)
                    && WinHttpReceiveResponse(req, NULL))
            {
                DWORD code = 0, sz = sizeof(code);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                                    WINHTTP_NO_HEADER_INDEX);
                std::string reply;
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(req, &avail) && avail > 0)
                {
                    std::string chunk(avail, '\0');
                    DWORD got = 0;
                    if (!WinHttpReadData(req, &chunk[0], avail, &got) || got == 0)
                        break;
                    reply.append(chunk, 0, got);
                    if (reply.size() > (1u << 20)) break;
                }

                if (code == 200)
                {
                    url = jsonValueOf(reply, "url");
                    if (url.empty())
                        why = "reply carried no url: " + reply.substr(0, 200);
                }
                else
                {
                    std::string e = jsonValueOf(reply, "error");
                    why = "server returned HTTP " + std::to_string(code)
                        + (e.empty() ? "" : " - " + e);
                }
            }
            else
            {
                why = "request failed (WinHTTP error "
                    + std::to_string(GetLastError()) + ")";
            }
            WinHttpCloseHandle(req);
        }
        else why = "WinHttpOpenRequest failed";
        WinHttpCloseHandle(con);
    }
    else why = "could not connect to " + wideToUtf8(host);

    WinHttpCloseHandle(ses);
    return url;
}

static void copyToClipboard(const std::wstring& text)
{
    if (!OpenClipboard(NULL))
        return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h)
    {
        void* p = GlobalLock(h);
        if (p)
        {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
        else
        {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

// Offline self-test of the upload path: stage an existing archive and log the
// URL the server returns, without opening a browser.
//   tweb.exe --stage <path_to_zip>
static int stageOnly()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 3)
    {
        logLine("--stage needs <path_to_zip>");
        return 1;
    }
    std::wstring zip = argv[2];
    LocalFree(argv);

    Config cfg = loadConfig();
    if (!cfg.loaded)
    {
        logLine("STAGE CONFIG ERROR: " + cfg.problem);
        return 1;
    }

    size_t slash = zip.find_last_of(L"/\\");
    std::string name = wideToUtf8(slash == std::wstring::npos ? zip : zip.substr(slash + 1));

    std::string why;
    std::string url = stageArchive(cfg, zip, name, std::string(), siteLanguage(cfg.lang), why);
    if (url.empty())
    {
        logLine("STAGE FAILED: " + why);
        return 1;
    }
    logLine("STAGE OK\r\n  " + name + "\r\n  " + url);
    return 0;
}

// ---------------- first-run settings ----------------
//
// Shown once.  It holds the email, states that the design leaves the computer,
// and links to the agreement on the website.  After it has been accepted the
// button just uploads; --settings reopens it to change the address.
static std::string settingsPath()
{
    std::string appdata = env("APPDATA");
    if (appdata.empty())
        return std::string();
    std::string dir = appdata + "\\Tomachie";
    CreateDirectoryW(utf8ToWide(dir).c_str(), NULL);
    return dir + "\\tweb_user.json";
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
    std::string js = "{\n  \"email\": \"" + email + "\",\n  \"accepted\": true\n}\n";
    HANDLE fh = CreateFileW(utf8ToWide(p).c_str(), GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE)
        return;
    DWORD w = 0;
    WriteFile(fh, js.data(), DWORD(js.size()), &w, NULL);
    CloseHandle(fh);
}

struct DlgState
{
    const Strings* s = NULL;
    std::wstring   cdaUrl;
    std::wstring   learnUrl;
    std::wstring   email;
    bool           ok = false;
    HWND           edEmail = NULL;
    HWND           edConfirm = NULL;
    HFONT          font = NULL;
    HFONT          boldFont = NULL;
    HFONT          smallFont = NULL;
};

static LRESULT CALLBACK dlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    DlgState* st = (DlgState*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (msg)
    {
    // A link has to look clickable: give the two link statics the hand cursor.
    case WM_SETCURSOR:
        if (st && ((HWND)wp == GetDlgItem(h, 400) || (HWND)wp == GetDlgItem(h, 401)))
        {
            SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32649)));   // IDC_HAND
            return TRUE;
        }
        break;

    case WM_CTLCOLORSTATIC:
        if (st && ((HWND)lp == GetDlgItem(h, 400) || (HWND)lp == GetDlgItem(h, 401)))
        {                                               // the two links

            SetTextColor((HDC)wp, RGB(0, 90, 190));
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        if (st && (HWND)lp == GetDlgItem(h, 106))        // the legal notice
        {
            SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);

    case WM_COMMAND:
        if (!st) break;
        switch (LOWORD(wp))
        {
        case 400:                                        // CDA + privacy
            if (HIWORD(wp) == STN_CLICKED && !st->cdaUrl.empty())
                ShellExecuteW(NULL, L"open", st->cdaUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
            return 0;
        case 401:                                        // how it works
            if (HIWORD(wp) == STN_CLICKED && !st->learnUrl.empty())
                ShellExecuteW(NULL, L"open", st->learnUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
            return 0;
        case IDOK:
        {
            wchar_t a[512] = {0}, b[512] = {0};
            GetWindowTextW(st->edEmail, a, 511);
            GetWindowTextW(st->edConfirm, b, 511);
            if (wcscmp(a, b) != 0)
            {
                MessageBoxW(h, utf8ToWide(st->s->get("email_mismatch")).c_str(),
                            L"Tomachie", MB_ICONWARNING);
                return 0;
            }
            if (a[0] == 0 || !wcschr(a, L'@'))
                return 0;                                // nothing useful entered
            st->email = a;
            st->ok = true;
            DestroyWindow(h);
            return 0;
        }
        case IDCANCEL:
            DestroyWindow(h);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y,
               int w, int hgt, HWND parent, int id, HFONT font)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, hgt, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(NULL), NULL);
    if (c && font)
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

// Returns true if the user accepted; email carries the address.
static bool showSettings(const Strings& s, const std::wstring& cdaUrl,
                         const std::wstring& learnUrl,
                         const std::string& projectName, std::string& email)
{
    static const wchar_t* CLS = L"TwebSettings";
    HINSTANCE inst = GetModuleHandleW(NULL);

    // Embedded icon (resource id 1), so the title bar, the Alt-Tab list and the
    // taskbar all show the Tomachie mark instead of the default application
    // icon.  Loaded at the two sizes Windows asks for rather than letting it
    // stretch one.
    HICON icoBig   = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXICON),
                                       GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON icoSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON),
                                       GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = dlgProc;
    wc.hInstance     = inst;
    wc.hIcon         = icoBig;
    wc.hCursor       = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLS;
    RegisterClassW(&wc);

    NONCLIENTMETRICSW ncm;
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);

    DlgState st;
    st.s        = &s;
    st.cdaUrl   = cdaUrl;
    st.learnUrl = learnUrl;

    DWORD exStyle = isRightToLeft(s.code) ? WS_EX_LAYOUTRTL : 0;
    std::wstring title = utf8ToWide(formatOne(s.get("title"), projectName));

    const int W = 560, H = 478;
    HWND h = CreateWindowExW(exStyle, CLS, title.c_str(),
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, W, H,
                             NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!h)
        return false;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);

    // The class icon alone does not always take effect for an already-created
    // window, so set both explicitly.
    if (icoBig)   SendMessageW(h, WM_SETICON, ICON_BIG,   (LPARAM)icoBig);
    if (icoSmall) SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)icoSmall);

    // Bold heading, then the explanation, then the off-site notice.
    LOGFONTW bf = ncm.lfMessageFont;
    bf.lfWeight = FW_BOLD;
    HFONT boldFont = CreateFontIndirectW(&bf);
    st.boldFont = boldFont;

    mk(L"STATIC", utf8ToWide(s.get("heading")).c_str(), 0, 16, 14, W - 60, 20, h, 99, boldFont);
    mk(L"STATIC", utf8ToWide(s.get("intro")).c_str(),   0, 16, 40, W - 60, 36, h, 100, font);
    mk(L"STATIC", utf8ToWide(formatOne(s.get("offsite"), TOMACHIE_HOST)).c_str(),
       0, 16, 80, W - 60, 36, h, 103, font);
    mk(L"STATIC", utf8ToWide(s.get("save_first")).c_str(),
       0, 16, 120, W - 60, 20, h, 105, font);

    mk(L"STATIC", utf8ToWide(s.get("email")).c_str(), 0, 16, 152, 200, 18, h, 101, font);
    st.edEmail = mk(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                    16, 172, W - 60, 24, h, 200, font);

    mk(L"STATIC", utf8ToWide(s.get("confirm_email")).c_str(), 0, 16, 204, 200, 18, h, 102, font);
    st.edConfirm = mk(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                      16, 224, W - 60, 24, h, 201, font);

    // Reopened from Shift+click: show the address already in use, or the user
    // cannot see what they set and has to guess.
    if (!email.empty())
    {
        std::wstring cur = utf8ToWide(email);
        SetWindowTextW(st.edEmail,   cur.c_str());
        SetWindowTextW(st.edConfirm, cur.c_str());
    }

    mk(L"STATIC", utf8ToWide(s.get("cda_link")).c_str(), SS_NOTIFY,
       16, 260, W - 60, 20, h, 400, font);
    mk(L"STATIC", utf8ToWide(s.get("learn_link")).c_str(), SS_NOTIFY,
       16, 284, W - 60, 20, h, 401, font);

    // How to get back here later - this dialog is shown once.
    mk(L"STATIC", utf8ToWide(s.get("settings_hint")).c_str(), 0,
       16, 316, W - 60, 36, h, 104, font);

    mk(L"BUTTON", utf8ToWide(s.get("send")).c_str(),
       BS_DEFPUSHBUTTON | WS_TABSTOP, W - 320, 366, 140, 30, h, IDOK, font);
    mk(L"BUTTON", utf8ToWide(s.get("cancel")).c_str(),
       WS_TABSTOP, W - 170, 366, 120, 30, h, IDCANCEL, font);

    // Legal notice: deliberately hard-coded and English-only, and deliberately
    // NOT a token - it must not vary by language or be editable in the i18n
    // file.  This is the one string in the dialog that is not translated.
    LOGFONTW sf = ncm.lfMessageFont;
    sf.lfHeight = LONG(sf.lfHeight * 0.85);
    HFONT smallFont = CreateFontIndirectW(&sf);
    st.smallFont = smallFont;
    mk(L"STATIC", L"Tweb " TWEB_VERSION_W L"  (c) 2026 Tomachie LLC.  MIT License.", 0,
       16, 404, W - 60, 16, h, 106, smallFont);

    // KiCad launches plugins with a STARTUPINFO show-state, and Windows makes
    // the FIRST ShowWindow call in a process ignore its argument and use the
    // launcher's instead - which left this dialog created but invisible, with
    // the message loop below waiting forever.  SetWindowPos is not subject to
    // that rule, and the redundant ShowWindow absorbs it if it ever is.
    ShowWindow(h, SW_SHOW);
    SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    ShowWindow(h, SW_SHOW);

    // Raise above KiCad without staying permanently on top.
    SetWindowPos(h, HWND_TOPMOST,   0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    SetFocus(st.edEmail);

    if (!IsWindowVisible(h))
        logLine("settings dialog is not visible after show - aborting rather than hanging");

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0))
    {
        if (!IsDialogMessageW(h, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    DeleteObject(font);
    if (boldFont) DeleteObject(boldFont);
    if (smallFont) DeleteObject(smallFont);

    if (st.ok)
        email = wideToUtf8(st.email);
    return st.ok;
}

// The API-disabled message, localized. Defined here because it needs Config
// and Strings, which are declared below run().
static std::string apiDisabledMessage()
{
    Config c = loadConfig();
    Strings s = loadStrings(c.loaded ? c.lang : std::string());
    return s.get("api_disabled");
}

// Reopen the settings dialog to change the stored address.
//   tweb.exe --settings
static int settingsOnly()
{
    Config cfg = loadConfig();
    Strings s  = loadStrings(cfg.loaded ? cfg.lang : std::string());
    std::string email = loadSavedEmail();          // prefill with what is in use
    if (!showSettings(s, utf8ToWide(withLang(urlFor(cfg, cfg.cdaPath), siteLanguage(cfg.lang))), utf8ToWide(withLang(urlFor(cfg, cfg.learnPath), siteLanguage(cfg.lang))), "settings", email))
    {
        logLine("SETTINGS cancelled");
        return 1;
    }
    saveEmail(email);
    logLine("SETTINGS saved: " + email + "  (language " + s.code + ")");
    return 0;
}

static void revealInExplorer(const std::wstring& zipPath)
{
    copyToClipboard(zipPath);
    std::wstring args = L"/select,\"" + zipPath + L"\"";
    ShellExecuteW(NULL, L"open", L"explorer.exe", args.c_str(), NULL, SW_SHOWNORMAL);
}

// Stage the archive, then open the URL the server returns.  The page fills
// itself in from the token; the person picks the options and presses Analyze,
// which is the only thing that creates a job.
static void handOffToBrowser(const std::wstring& zipPath, std::ostringstream& rpt)
{
    Config cfg = loadConfig();
    if (!cfg.loaded)
    {
        rpt << "\nCONFIGURATION MISSING\n  " << cfg.problem << "\n"
            << "\nThe archive is selected in Explorer and its path is on the\n"
               "clipboard, so it can be uploaded by hand.\n";
        revealInExplorer(zipPath);
        return;
    }

    size_t slash = zipPath.find_last_of(L"/\\");
    std::string zipName = wideToUtf8(slash == std::wstring::npos
                                     ? zipPath : zipPath.substr(slash + 1));

    Strings     s    = loadStrings(cfg.lang);        // dialog text (may be the EN fallback)
    std::string site = siteLanguage(cfg.lang);        // website language (never falls back)

    // First run only: email, the off-site notice and the agreement link.
    // Once accepted, the button just uploads.
    std::string email = loadSavedEmail();
    if (email.empty())
    {
        if (!showSettings(s, utf8ToWide(withLang(urlFor(cfg, cfg.cdaPath), site)),
                          utf8ToWide(withLang(urlFor(cfg, cfg.learnPath), site)),
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
        ShellExecuteW(NULL, L"open", utf8ToWide(url).c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
    else
    {
        rpt << "\nUPLOAD FAILED\n  " << why << "\n\n"
            << "The archive is selected in Explorer and its path is on the\n"
               "clipboard, so it can still be uploaded by hand.\n";
        revealInExplorer(zipPath);
    }
}

static void crashReport(unsigned long code)
{
    char msg[128];
    wsprintfA(msg, "tweb CRASHED with exception code 0x%08X", code);
    logLine(msg);
    MessageBoxA(NULL, msg, "Tomachie", MB_ICONERROR);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    __try { return run(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { crashReport(GetExceptionCode()); return 2; }
}
