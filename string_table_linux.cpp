// StringTable, Linux side: same flat format as upstream, OS language read
// from the locale environment instead of the Win32 locale API.
#include "string_table.h"

#include <cstdlib>
#include <string>

static std::string trimmed(std::string s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    while (!s.empty() && s.front() == ' ')
        s.erase(s.begin());
    return s;
}

static std::string expandLineBreaks(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n')
        {
            out += '\n';
            ++i;
        }
        else
            out += s[i];
    }
    return out;
}

bool StringTable::parse(const std::string& text)
{
    size_t i = 0;
    while (i < text.size())
    {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string line = trimmed(text.substr(i, e - i));
        i = e + 1;
        if (line.empty() || line[0] == ';')
            continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        items.push_back(std::make_pair(trimmed(line.substr(0, eq)),
                                       expandLineBreaks(trimmed(line.substr(eq + 1)))));
    }
    return !items.empty();
}

std::string StringTable::get(const std::string& token) const
{
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].first == token)
            return items[i].second;
    return token;
}

std::string StringTable::format(const std::string& token, const std::string& arg) const
{
    std::string text = get(token);
    size_t k = text.find("{0}");
    if (k != std::string::npos)
        text = text.substr(0, k) + arg + text.substr(k + 3);
    return text;
}

// Chinese needs the script, not just the language, to pick between simplified
// and traditional. Reads LANGUAGE (first entry wins), then LC_ALL,
// LC_MESSAGES, then LANG: e.g. "de_DE.UTF-8" -> "de", "zh_TW" -> "tw".
std::string StringTable::osLanguageCode()
{
    std::string raw;
    const char* language = getenv("LANGUAGE");
    if (language && *language)
    {
        raw = language;
        size_t c = raw.find(':');
        if (c != std::string::npos)
            raw = raw.substr(0, c);
    }
    if (raw.empty())
    {
        const char* lc = getenv("LC_ALL");
        if (!lc || !*lc) lc = getenv("LC_MESSAGES");
        if (!lc || !*lc) lc = getenv("LANG");
        if (lc) raw = lc;
    }
    std::string low;
    for (size_t i = 0; i < raw.size(); ++i)
    {
        char ch = raw[i];
        if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
        low += ch;
    }
    if (low.empty() || low == "c" || low == "posix")
        return "en";
    // Strip encoding/modifier: "de_de.utf-8@euro" -> "de_de".
    size_t dot = low.find('.');
    if (dot != std::string::npos) low = low.substr(0, dot);
    size_t at = low.find('@');
    if (at != std::string::npos) low = low.substr(0, at);

    if (low.compare(0, 3, "zh_") == 0 || low == "zh")
    {
        bool traditional = low.find("tw") != std::string::npos
                        || low.find("hk") != std::string::npos
                        || low.find("mo") != std::string::npos
                        || low.find("hant") != std::string::npos;
        return traditional ? "tw" : "cn";
    }
    std::string iso = low.substr(0, low.find('_'));
    if (iso == "ko") return "kr";
    if (iso == "vi") return "vn";
    if (iso == "iw" || iso == "he") return "he";
    if (iso.size() == 2)
        return iso;
    return "en";
}
