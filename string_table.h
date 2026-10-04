// StringTable - the flat "token = string" dialog strings (i18n/tweb_<code>.txt)
// shared by tweb.exe and the setup program.
//
// One token per line, ';' starts a comment line, the text RIGHT of the first
// '=' is the string.  "\n" in a value is a line break, so a multi-line message
// still occupies one line of the file.
#pragma once

#include <string>
#include <utility>
#include <vector>

class StringTable
{
public:
    std::string code = "en";
    std::vector<std::pair<std::string, std::string>> items;

    // Parses the file text.  False when it holds no token at all.
    bool parse(const std::string& text);

    // The string for a token; the token itself when absent - visible, not silent.
    std::string get(const std::string& token) const;

    // get() with {0} replaced by arg.
    std::string format(const std::string& token, const std::string& arg) const;

    // The OS user language as one of Tomachie's codes (cn/tw/kr/vn/he differ
    // from ISO; the rest are ISO 639-1 already).
    static std::string osLanguageCode();
};
