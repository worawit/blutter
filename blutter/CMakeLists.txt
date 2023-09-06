cmake_minimum_required (VERSION 3.19)

project(blutter)
if (NOT DEFINED DARTLIB)
	message(FATAL_ERROR "Please define DARTLIB to build the Blutter project")
endif()
if (NOT ${DARTLIB} MATCHES "^dartvm")
	message(FATAL_ERROR "DARTLIB name should start with 'dartvm'")
endif()

set(BINNAME "${PROJECT_NAME}_${DARTLIB}")

set(CMAKE_INSTALL_PREFIX "${PROJECT_SOURCE_DIR}/../bin" CACHE PATH "" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
	# for macOS only
	# <format> library is experimental in clang 15
	add_compile_options(-fexperimental-library)
	# need an extra lib for experimental library
	add_link_options(-fexperimental-library -L$ENV{HOMEBREW_PREFIX}/opt/llvm@15/lib/c++ -dead_strip)
endif()

find_package(${DARTLIB} REQUIRED PATHS "${PROJECT_SOURCE_DIR}/../packages")
if (MSVC)
	add_library(capstone SHARED IMPORTED)
	set_target_properties(capstone PROPERTIES IMPORTED_LOCATION "${PROJECT_SOURCE_DIR}/../external/capstone/capstone.dll")
	set_target_properties(capstone PROPERTIES IMPORTED_IMPLIB "${PROJECT_SOURCE_DIR}/../external/capstone/capstone_dll.lib")
	target_include_directories(capstone INTERFACE "${PROJECT_SOURCE_DIR}/../external/capstone/include/capstone")
else()
	include(FindPkgConfig)
	pkg_search_module(CAPSTONE REQUIRED capstone)
	link_directories(${CAPSTONE_LIBDIR})
	include_directories(AFTER ${CAPSTONE_INCLUDEDIR})
endif()

# Add source to this project's executable.
set(SRCDIR "src")
include(sourcelist.cmake)
list(TRANSFORM SRCS PREPEND "${SRCDIR}/")
add_executable(${BINNAME} ${SRCS})

target_link_libraries(${BINNAME} ${DARTLIB} capstone)

target_precompile_headers(${BINNAME} PRIVATE "${SRCDIR}/pch.h")

if (MSVC)
	# remove compiler exception handler options
	#string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
	# remove compiler RTTI option
	string(REPLACE "/GR" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
	#set(cc_opts /Oy /GR- /EHs-c-)
	if (CMAKE_GENERATOR MATCHES "Visual Studio")
		# use SEMIDBG for enabling ASSERT macro while linking against release build of Dart VM
		set(cc_opts /GR- /MD /D SEMIDBG)
		set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${BINNAME})
		set_target_properties(${BINNAME} PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS "${DBG_CMD}")
	else()
		# assume Ninja
		set(cc_opts /Oy /GR- /sdl- /Oi /GL /Gy /Zc:wchar_t /Zc:inline)
		target_link_options(${BINNAME} PRIVATE /LTCG /OPT:REF /OPT:ICF)
	endif()

else()
	# TODO: gcc options to remove dead code in static dartvm library
	set(cc_opts
		-O3 -fno-rtti
		-fvisibility=hidden -fvisibility-inlines-hidden -fno-omit-frame-pointer
		#-Wl,--gc-sections
		#-fno-exceptions -Wall
	)

endif()

get_filename_component(FRIDA_TEMPLATE_DIR "${PROJECT_SOURCE_DIR}/../scripts/" ABSOLUTE)
set(defines NDEBUG FRIDA_TEMPLATE_DIR="${FRIDA_TEMPLATE_DIR}")
if (OLD_MAP_SET_NAME)
	set(defines ${defines} OLD_MAP_SET_NAME)
endif()
if (HAS_SHARED_CLASS_TABLE)
	set(defines ${defines} HAS_SHARED_CLASS_TABLE)
endif()
if (HAS_TYPE_REF)
	set(defines ${defines} HAS_TYPE_REF)
endif()
if (HAS_RECORD_TYPE)
	set(defines ${defines} HAS_RECORD_TYPE)
endif()
target_compile_definitions(${BINNAME} PRIVATE ${defines})

target_compile_options(${BINNAME} PRIVATE ${cc_opts})

install (TARGETS ${BINNAME} RUNTIME DESTINATION "${PROJECT_SOURCE_DIR}/../bin")
