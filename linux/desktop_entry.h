#pragma once

#include <string>

namespace clickshade::linuxapp::detail {

// Exec has two escaping layers: Desktop Entry string decoding happens before
// the command's double-quoted argument is parsed. Preserve literal characters
// through both layers, including percent field-code syntax.
inline std::string QuoteDesktopExecArgument(const std::string& value) {
    std::string argument = "\"";
    for (char c : value) {
        if (c == '%') { argument += "%%"; continue; }
        if (c == '\\' || c == '"' || c == '`' || c == '$') argument += '\\';
        argument += c;
    }
    argument += '"';
    std::string encoded;
    for (char c : argument) {
        switch (c) {
        case '\\': encoded += "\\\\"; break;
        case '\n': encoded += "\\n"; break;
        case '\r': encoded += "\\r"; break;
        case '\t': encoded += "\\t"; break;
        default: encoded += c; break;
        }
    }
    return encoded;
}

} // namespace clickshade::linuxapp::detail
