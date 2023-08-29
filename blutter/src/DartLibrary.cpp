#include "pch.h"
#include "DartLibrary.h"
#include "DartClass.h"
#include <filesystem>

DartLibrary::DartLibrary(const dart::Library& lib) : ptr(lib.ptr()), topClass(NULL)
{
	auto& dtext = dart::String::Handle();
	dtext = lib.name();
	name = dtext.ToCString();
	dtext = lib.url();
	url = dtext.ToCString();

	if (!name.empty() || lib.is_dart_scheme())
		isInternal = true;
	//else if (url.starts_with("package:flutter/src/") || url.starts_with("package:typed_data/src/") || url.starts_with("package:collection/src/"))
	//	isInternal = true;
	else
		isInternal = false;

	// add all classes belong to this library
	auto& cls = dart::Class::Handle();
	cls = lib.toplevel_class();
	topClass = AddClass(cls);
	id = topClass->Id();

	dart::DictionaryIterator iter(lib);
	while (iter.HasNext()) {
		auto objPtr = iter.GetNext();
		// only 4 possible types but functions and fields are in top level class
		if (objPtr.IsClass()) {
			cls = dart::Class::RawCast(objPtr);
			AddClass(cls);
		}
		else if (objPtr.IsFunction()) {
			// TODO: check if top level class contain this function
		}
		else if (objPtr.IsField()) {
			// TODO: check if top level class contain this field
		}
		else if (objPtr.IsLibraryPrefix()) {
			throw std::runtime_error("library prefix in AOT");
		}
		else {
			throw std::runtime_error("unknown type in library");
		}
	}
}

DartLibrary::~DartLibrary()
{
	for (auto cls : classes) {
		delete cls;
	}
}

std::string DartLibrary::GetName()
{
	// name is empty for non-internal dart lib
	std::string out;
	if (url.starts_with("package:")) {
		//out = url.substr(8, url.find('/', 8) - 8);
		out = url.substr(8);
	}
	else if (url.starts_with("file:")) {
		auto offset = url.rfind('/');
		offset = url.rfind('/', offset - 1);
		out = url.substr(offset + 1);
	}
	else {
		// expect "dart:*"
		out = url;
		out[4] = '_';
	}

	if (out.ends_with(".dart"))
		out.erase(out.length() - 5);
	std::replace(out.begin(), out.end(), '/', '$');

	return out;
}

DartClass* DartLibrary::AddClass(const dart::Class& cls)
{
	auto dartCls = new DartClass(*this, cls);
	classes.push_back(dartCls);
	return dartCls;
}

std::string DartLibrary::CreatePath(const char* base_dir)
{
	// create subdirectories for the library file
	std::string path = base_dir;
	path.push_back('/');
	size_t start_pos = 0;
	if (url.starts_with("package:")) {
		start_pos = 8; // skip "package:"
	}
	else if (url.starts_with("file:///")) {
		start_pos = url.find("/.dart_tool/") + 1;
	}
	else {
		ASSERT(url.starts_with("dart:"));
		path.append("dart");
		std::filesystem::create_directories(path);
		return path.append("/").append(&url[5]).append(".dart");
	}
	const char* lib_path = &url[start_pos]; // skip directory name
	const char* end = strrchr(lib_path, '/');
	path.append(lib_path, end);
	std::filesystem::create_directories(path);
	path.append(end);
	return path;
}

void DartLibrary::PrintCommentInfo(std::ostream& of)
{
	of << std::format("// lib: {}, url: {}\n", name.c_str(), url.c_str());
}
