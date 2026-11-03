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

	if (lib.is_dart_scheme())
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

// The library url comes from the snapshot, so a crafted libapp can use one such as "package:../../x"
// to write outside the output directory. Split on both separators and drop the empty, "." and ".." parts
// (and parts of only dots and spaces, which Windows trims to "." or "..").
static std::vector<std::string> safePathParts(std::string_view path)
{
	std::vector<std::string> parts;
	size_t start = 0;
	while (start <= path.size()) {
		auto end = path.find_first_of("/\\", start);
		if (end == std::string_view::npos)
			end = path.size();
		const auto part = path.substr(start, end - start);
		if (part.find_first_not_of(". ") != std::string_view::npos)
			parts.emplace_back(part);
		start = end + 1;
	}
	return parts;
}

std::string DartLibrary::CreatePath(const char* base_dir)
{
	// create subdirectories for the library file
	std::string_view lib_path = url;
	std::vector<std::string> parts;
	std::string ext;
	if (url.starts_with("package:")) {
		lib_path.remove_prefix(8); // skip "package:"
	}
	else if (url.starts_with("file:///")) {
		const auto pos = url.find("/.dart_tool/");
		if (pos != std::string::npos)
			lib_path.remove_prefix(pos + 1);
	}
	else if (url.starts_with("dart:")) {
		lib_path.remove_prefix(5);
		parts.push_back("dart");
		ext = ".dart";
	}
	else {
		// obfuscated
		ext = ".dart";
	}
	for (auto& part : safePathParts(lib_path))
		parts.push_back(std::move(part));
	if (parts.empty()) {
		parts.push_back(std::format("lib_{}", id));
		ext = ".dart";
	}

	std::string path = base_dir;
	for (size_t i = 0; i + 1 < parts.size(); i++)
		path.append("/").append(parts[i]);
	std::filesystem::create_directories(path);
	return path.append("/").append(parts.back()).append(ext);
}

void DartLibrary::PrintCommentInfo(std::ostream& of)
{
	of << std::format("// lib: {}, url: {}\n", name.c_str(), url.c_str());
}
