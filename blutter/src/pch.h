// pch.h: This is a precompiled header file.
// Files listed below are compiled only once, improving build performance for future builds.
// This also affects IntelliSense performance, including code completion and many code browsing features.
// However, files listed here are ALL re-compiled if any one of them is updated between builds.
// Do not add files here that you will be updating frequently as this negates the performance advantage.

#ifndef PCH_H
#define PCH_H

// add headers that you want to pre-compile here
#include <string>
#include <format>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <iostream>

#if defined(_MSC_VER)
#	define PRAGMA_WARNING(...) __pragma(warning(__VA_ARGS__))
#else
#	define PRAGMA_WARNING(...) 
#endif

PRAGMA_WARNING(push, 0)
#include <include/dart_api.h>
#include <vm/dart_entry.h>
#include <vm/dart.h>
#include <vm/object.h>
#include <vm/object_store.h>
//#include <vm/stub_code.h>
#include <vm/native_symbol.h>
//#include <vm/field_table.h>
//#include <vm/canonical_tables.h>
#include <vm/zone_text_buffer.h>
#include <vm/tagged_pointer.h>
#include <vm/compiler/runtime_api.h>
#include <vm/compiler/runtime_offsets_extracted.h>
PRAGMA_WARNING(pop)

#ifdef OLD_MAP_SET_NAME
namespace dart {
	using Map = LinkedHashMap;
	using Set = LinkedHashSet;
#ifdef OLD_MAP_NO_IMMUTABLE
	using ConstMap = LinkedHashMap;
	using ConstSet = LinkedHashSet;
#else
	using ConstMap = ImmutableLinkedHashMap;
	using ConstSet = ImmutableLinkedHashSet;
#endif
	
	enum ClassIdX : intptr_t {
		kMapCid = kLinkedHashMapCid,
		kSetCid = kLinkedHashSetCid,
#ifdef OLD_MAP_NO_IMMUTABLE
		kConstMapCid = kLinkedHashMapCid,
		kConstSetCid = kLinkedHashSetCid,
#else
		kConstMapCid = kImmutableLinkedHashMapCid,
		kConstSetCid = kImmutableLinkedHashSetCid,
#endif
	};
};
#endif

#ifdef NO_LAST_INTERNAL_ONLY_CID
namespace dart {
	constexpr intptr_t kLastInternalOnlyCid = kUnwindErrorCid;
};
#endif

#if defined SEMIDBG && !defined DEBUG
// the debug build configuration that use release configuration but no optimization
// so only the executable are fully debuggable. but ASSERT is gone
#undef ASSERT
#define ASSERT(cond) RELEASE_ASSERT(cond)
// Note: DEBUG_ASSERT requires dart vm is built with debug mode because it might access some field that exists only in DEBUG
//#undef DEBUG_ASSERT
//#define DEBUG_ASSERT(cond) RELEASE_ASSERT(cond)
#endif // defined SEMIDBG && !defined DEBUG

// https://github.com/dart-lang/sdk/commit/bf4bb953081f11fbec9a1c0ea08b428c744b369e
// new async/await implementation in Dart (around version 2.16 or 2.17)
// the result is generated code in async and prologue is changed drastically
//   now, analyzer can only work against new implementation
#ifdef CACHED_FUNCTION_ENTRY_POINTS_LIST
#  define HAS_INIT_ASYNC 1
#endif

// InitLateStaticFieldStub is implemented around Dart 2.16. Before that, only InitStaticFieldStub
#ifdef NO_INIT_LATE_STATIC_FIELD
#  define InitLateStaticFieldStub InitStaticFieldStub
#  define InitLateFinalStaticFieldStub InitStaticFieldStub
#endif

// https://github.com/dart-lang/sdk/commit/84fd647969f0d74ab63f0994d95b5fc26cac006a
// refactor access to integer value to be same name "Value()". Basically, only dart::Mint is changed.
// in dart 3.6, there are many internal changes (below). just use only one macro below for simplicity.
//   all of them are commited separately in dev only. so, it does not work only some dev version.
// - Improve BitField API. UntaggedObject use BitField. kXXXPos and kXXXSize are changed to XXX.
//   Use XXX::shift() and XXX::bitsize() for Pos and Size respectively
// https://github.com/dart-lang/sdk/commit/d3c165d7b52e48672224d0e46d2c74696fd89322
// - Record::GetRecordType() with one argument
// https://github.com/dart-lang/sdk/commit/ab19361e87a0248ade1e883f638542163f9100d1
#ifdef UNIFORM_INTEGER_ACCESS
#  define MintValue(obj) obj.Value()
	constexpr intptr_t kUntaggedObjectClassIdTagPos = dart::UntaggedObject::ClassIdTag::shift();
#  define DartGetRecordType(record) record.GetRecordType(dart::TypeVisibility::kUserVisibleType)
#else
#  define MintValue(obj) obj.value()
	constexpr intptr_t kUntaggedObjectClassIdTagPos = dart::UntaggedObject::kClassIdTagPos;
#  define DartGetRecordType(record) record.GetRecordType()
#endif

#endif //PCH_H
