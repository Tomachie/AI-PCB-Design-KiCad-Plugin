#include "string_table.h"

#include <windows.h>

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
// and traditional.
std::string StringTable::osLanguageCode()
{
    wchar_t buf[LOCALE_NAME_MAX_LENGTH] = {0};
    if (!GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH))
        return "en";
    std::string name;                                // e.g. "de-de", "zh-hant-tw"
    for (const wchar_t* p = buf; *p; ++p)
        name += char(*p < 128 ? towlower(*p) : '?');

    std::string iso = name.substr(0, name.find('-'));
    if (iso == "zh")
    {
        bool traditional = name.find("hant") != std::string::npos
                        || name.find("-tw")  != std::string::npos
                        || name.find("-hk")  != std::string::npos
                        || name.find("-mo")  != std::string::npos;
        return traditional ? "tw" : "cn";
    }
    if (iso == "ko") return "kr";
    if (iso == "vi") return "vn";
    if (iso == "iw") return "he";                   // legacy code for Hebrew
    return iso;
}
