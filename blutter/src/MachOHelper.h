#pragma once
// LibAppInfo is shared with the ELF loader.
#include "ElfHelper.h"

// Finds the Dart AOT snapshots in a Mach-O image, i.e. the "App" binary inside
// the App.framework of an iOS/macOS Flutter application.
class MachOHelper final
{
public:
	// Cheap magic check, so a caller can pick between this and ElfHelper.
	static bool IsMachO(const char* path);
	static LibAppInfo MapLibApp(const char* path);

private:
	MachOHelper() = delete;
};
