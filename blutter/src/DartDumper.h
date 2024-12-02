#pragma once
#include "DartApp.h"
#include <filesystem>

class DartDumper
{
public:
	DartDumper(DartApp& app) : app(app) {};

	void Dump4Ida(std::filesystem::path outDir);
	void Dump4Radare2(std::filesystem::path outDir);

	std::vector<std::pair<intptr_t, std::string>> DumpStructHeaderFile(std::string outFile);

	void DumpCode(const char* out_dir);

	void DumpObjectPool(const char* filename);
	void DumpObjects(const char* filename);

	std::string ObjectToString(dart::Object& obj, bool simpleForm = false, bool nestedObj = false, int depth = 0);

private:
	std::string getPoolObjectDescription(intptr_t offset, bool simpleForm = true);

	std::string dumpInstance(dart::Object& obj, bool simpleForm = false, bool nestedObj = false, int depth = 0);
	std::string dumpInstanceFields(dart::Object& obj, DartClass& dartCls, intptr_t ptr, intptr_t offset, bool simpleForm = false, bool nestedObj = false, int depth = 0);

	void applyStruct4Ida(std::ostream& of);

	const std::string& getQuoteString(dart::Object& obj);

	DartApp& app;
	// map for object ptr to unescape string with quote
	std::unordered_map<intptr_t, std::string> quoteStringCache;
};
