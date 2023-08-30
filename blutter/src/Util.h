#pragma once
class Util
{
public:
	static std::string Unescape(const std::string& s);
	static std::string Unescape(const char* s);
	static std::string UnescapeWithQuote(const char* s);
	static std::string Quote(const std::string& s);
	static std::string Unquote(const std::string& s);
};

