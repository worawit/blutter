# 1
# this is CMakeLists.txt template file for building dart AOT VM
cmake_minimum_required (VERSION 3.20)

project(dartvmVERSION_PLACE_HOLDER)
if (${PROJECT_NAME} MATCHES "_PLACE_HOLDER")
	message(FATAL_ERROR "Do NOT use this file directly")
endif()
if(${CMAKE_SIZEOF_VOID_P} STREQUAL 4)
	message(FATAL_ERROR "Only 64-bit compiler here")
endif()

set(CMAKE_INSTALL_PREFIX "${PROJECT_SOURCE_DIR}/../../packages" CACHE PATH "" FORCE)

if (MSVC)
	set(ICU_ROOT "${PROJECT_SOURCE_DIR}/../../external/icu-windows")
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)
	execute_process(COMMAND brew --prefix icu4c OUTPUT_VARIABLE BREW_ICU)
	string(STRIP "${BREW_ICU}" ICU_ROOT)
endif()
find_package(ICU REQUIRED uc)

string(SUBSTRING ${PROJECT_NAME} 6 -1 DART_VERSION)

if (NOT DEFINED TARGET_OS)
	set(TARGET_OS "android")
endif()
if (NOT "${TARGET_OS}" MATCHES "^(android|ios)$")
	message(FATAL_ERROR "Only android or ios platform")
endif()

if (NOT DEFINED TARGET_ARCH)
	set(TARGET_ARCH "arm64")
endif()
if (NOT "${TARGET_ARCH}" MATCHES "^(arm64|x64)$")
	message(FATAL_ERROR "Only arm64 or x64 architecture")
endif()

set(SRCDIR "runtime")
set(LIBNAME "dartvm${DART_VERSION}_${TARGET_OS}_${TARGET_ARCH}")

set(CMAKE_DEBUG_POSTFIX "_d")

# Add source to this project's executable.
include(sourcelist.cmake)
add_library(${LIBNAME} STATIC ${SRCS})

# use C++17 because compilation with c++20 is slower
set_target_properties(${LIBNAME} PROPERTIES
	CXX_STANDARD 17
	CXX_STANDARD_REQUIRED True
	CXX_VISIBILITY_PRESET hidden
	CXX_STANDARD_REQUIRED ON
	VISIBILITY_INLINES_HIDDEN ON
	POSITION_INDEPENDENT_CODE ON)

# DART_TARGET_OS_ANDROID or DART_TARGET_OS_MACOS_IOS
if (${TARGET_OS} STREQUAL "android")
	set(target_os "DART_TARGET_OS_ANDROID")
	# Note: DART_COMPRESSED_POINTERS should be from the binary
	#set(dart_pointers "DART_COMPRESSED_POINTERS")
else()
	set(target_os "DART_TARGET_OS_MACOS_IOS")
endif()
# TARGET_ARCH_ARM64 or TARGET_ARCH_X64
if (${TARGET_ARCH} STREQUAL "arm64")
	set(target_arch "TARGET_ARCH_ARM64")
else()
	set(target_arch "TARGET_ARCH_X64")
endif()
if (${COMPRESSED_PTRS})
	set(dart_pointers "DART_COMPRESSED_POINTERS")
endif()

# DART_TARGET_OS_WINDOWS_UWP can be used to disable unused features on Windows such as native symbols lookup, Certificate Store
#   we never this it because we just want to load snapshot
# DART_SHARED_LIB U_ENABLE_DYLOAD=0 U_STATIC_IMPLEMENTATION -DFORCE_INCLUDE_DISASSEMBLER
set(defines ${target_arch} ${target_os}
	NDEBUG DART_PRECOMPILED_RUNTIME
	${dart_pointers} EXCLUDE_CFE_AND_KERNEL_PLATFORM
	PRODUCT U_USING_ICU_NAMESPACE=0
	_HAS_EXCEPTIONS=0 DART_TARGET_OS_WINDOWS_UWP
)
# Dart header can detect DART_HOST_OS_xxx value

if (MSVC)
	# remove compiler exception handler options
	string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
	# remove compiler RTTI option
	string(REPLACE "/GR" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
	set(cc_opts
		/Oy /GR- /EHs-c-
	)
else()
	set(cc_opts
		-O3 -fno-ident -fdata-sections -ffunction-sections
		-fno-omit-frame-pointer -fno-rtti -fno-exceptions
		-fno-sanitize=thread
		#-Wall
	)
endif()

target_compile_definitions(${LIBNAME} PUBLIC ${defines})

target_compile_options(${LIBNAME} PRIVATE ${cc_opts})
 
target_include_directories(${LIBNAME} PRIVATE "${SRCDIR}" "${ICU_INCLUDE_DIRS}")
target_include_directories(${LIBNAME} INTERFACE
							$<BUILD_INTERFACE:${SRCDIR}>
							$<INSTALL_INTERFACE:include/${PROJECT_NAME}>
						)

if (MSVC)
	target_link_libraries(${LIBNAME} PUBLIC ${ICU_LIBRARIES})
else()
	target_link_libraries(${LIBNAME} PUBLIC dl pthread ${ICU_LIBRARIES})
endif()

# install lib
set(CMAKE_INSTALL_MESSAGE LAZY)
# export name MUST be same as Config.cmake.in that is generated from dartvm_fetch_build.py
set(EXPORT_NAME "dartvmTarget")

install(TARGETS ${LIBNAME}
	EXPORT "${EXPORT_NAME}"
	DESTINATION lib
)

# install include files (all target OS and architecture uses same header files)
install(DIRECTORY "${SRCDIR}/"
	DESTINATION "include/${PROJECT_NAME}"
	FILES_MATCHING
	PATTERN "*.h"
	PATTERN "bin*" EXCLUDE
	PATTERN "docs*" EXCLUDE
	PATTERN "lib*" EXCLUDE
	PATTERN "observatory*" EXCLUDE
	PATTERN "tests*" EXCLUDE
	PATTERN "tools*" EXCLUDE
	PATTERN "third_party*" EXCLUDE
)

# install project cmake

message(STATUS ${CMAKE_BUILD_TYPE})
install(EXPORT "${EXPORT_NAME}"
	FILE "${EXPORT_NAME}.cmake"
	DESTINATION lib/cmake/${LIBNAME}
)


include(CMakePackageConfigHelpers)
# generate the config file that is includes the exports
set(CONFIG_CMAKE "${CMAKE_CURRENT_BINARY_DIR}/${LIBNAME}Config.cmake")
set(CONFIG_VERSION_CMAKE "${CMAKE_CURRENT_BINARY_DIR}/${LIBNAME}ConfigVersion.cmake")

configure_package_config_file(${CMAKE_CURRENT_SOURCE_DIR}/Config.cmake.in
	"${CONFIG_CMAKE}"
	INSTALL_DESTINATION "lib/cmake/${LIBNAME}"
	NO_SET_AND_CHECK_MACRO
	NO_CHECK_REQUIRED_COMPONENTS_MACRO
)

write_basic_package_version_file(
  "${CONFIG_VERSION_CMAKE}"
  VERSION "${DART_VERSION}"
  COMPATIBILITY ExactVersion
)

install(FILES
	"${CONFIG_CMAKE}"
	"${CONFIG_VERSION_CMAKE}"
	DESTINATION "lib/cmake/${LIBNAME}"
)
