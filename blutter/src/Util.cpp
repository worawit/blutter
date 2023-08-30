#include "pch.h"
#include "Util.h"
#include <sstream>
#include <iomanip>

// only ascii and no \0
static void unescape_char(std::string& s, char c)
{
    switch (c)
    {
    case '\a': s += "\\a"; break;
    case '\b': s += "\\b"; break;
    case '\f': s += "\\f"; break;
    case '\n': s += "\\n"; break;
    case '\r': s += "\\r"; break;
    case '\t': s += "\\t"; break;
    case '\v': s += "\\v"; break;
    case '\\': s += "\\\\"; break;
    case '\'': s += "\\'"; break;
    case '\"': s += "\\\""; break;
    case '\?': s += "\\\?"; break;
    default:   s += c; break;
    }
}


std::string Util::Unescape(const std::string& s)
{
	std::string res;
	res.reserve(s.length() * 2);
    for (char c : s) {
        unescape_char(res, c);
    }
	return res;
}

std::string Util::Unescape(const char* s)
{
    std::string res;
    res.reserve(strlen(s) * 2);
    while (char c = *s++) {
        unescape_char(res, c);
    }
    return res;
}

std::string Util::UnescapeWithQuote(const char* s)
{
    std::string res;
    res.reserve(strlen(s) * 2);
    res += '"';
    while (char c = *s++) {
        unescape_char(res, c);
    }
    res += '"';
    return res;
}

std::string Util::Quote(const std::string& s)
{
	std::ostringstream ss;
	ss << std::quoted(s);
	return ss.str();
}

std::string Util::Unquote(const std::string& s)
{
	std::string result;
	std::istringstream ss(s);
	ss >> std::quoted(result);
	return result;
}