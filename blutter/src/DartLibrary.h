#pragma once
#include <string>

class DartClass;

class DartLibrary
{
public:
	DartLibrary(const dart::Library& lib);
	// for creating non-existed library. needed for native classes
	DartLibrary(intptr_t nullId) : id((uint32_t)nullId), isInternal(true), ptr(nullptr), topClass(NULL) {}
	DartLibrary() = delete;
	DartLibrary(const DartLibrary&) = delete;
	DartLibrary(DartLibrary&&) = delete;
	DartLibrary& operator=(const DartLibrary&) = delete;
	~DartLibrary();

	bool operator==(const DartLibrary& rhs) {
		return name == rhs.name && url == rhs.url;
	}

	const std::string& Url() { return url; }
	std::string GetName();
	DartClass* AddClass(const dart::Class& cls);

	std::string CreatePath(const char* base_dir);
	void PrintCommentInfo(std::ostream& of);

	uint32_t id; // it is array index. store it in object to get the index quicker
	bool isInternal;
	const dart::LibraryPtr ptr;
	std::string name;
	std::string url;
	std::vector<DartClass*> classes;
	DartClass* topClass;
	// fields and functions are in topClass (named "::")
	//std::vector<DartField*> fields;
	//std::vector<DartFunction*> functions;
};

