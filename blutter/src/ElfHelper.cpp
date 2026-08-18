#include "pch.h"
#include "ElfHelper.h"
PRAGMA_WARNING(push, 0)
#include <platform/elf.h>
PRAGMA_WARNING(pop)
#include <cstring>
#include <stdexcept>
#if defined(_WIN32) || defined(WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif // #if defined(_WIN32) || defined(WIN32)

struct ElfIdent {
	uint8_t ei_magic[4];
	uint8_t ei_class;
	uint8_t ei_data;
	uint8_t ei_version;
	uint8_t ei_osabi;
	uint8_t ei_abiversion;
	uint8_t pad1[7];
};

struct MappedFile {
	uint8_t* data;
	size_t size;
};

using namespace dart::elf;

namespace macho {

#pragma pack(push, 1)

struct fat_header {
	uint32_t magic;
	uint32_t nfat_arch;
};

struct fat_arch {
	uint32_t cputype;
	uint32_t cpusubtype;
	uint32_t offset;
	uint32_t size;
	uint32_t align;
};

struct fat_arch_64 {
	uint32_t cputype;
	uint32_t cpusubtype;
	uint64_t offset;
	uint64_t size;
	uint32_t align;
	uint32_t reserved;
};

struct mach_header_64 {
	uint32_t magic;
	int32_t cputype;
	int32_t cpusubtype;
	uint32_t filetype;
	uint32_t ncmds;
	uint32_t sizeofcmds;
	uint32_t flags;
	uint32_t reserved;
};

struct load_command {
	uint32_t cmd;
	uint32_t cmdsize;
};

struct segment_command_64 {
	uint32_t cmd;
	uint32_t cmdsize;
	char segname[16];
	uint64_t vmaddr;
	uint64_t vmsize;
	uint64_t fileoff;
	uint64_t filesize;
	int32_t maxprot;
	int32_t initprot;
	uint32_t nsects;
	uint32_t flags;
};

struct section_64 {
	char sectname[16];
	char segname[16];
	uint64_t addr;
	uint64_t size;
	uint32_t offset;
	uint32_t align;
	uint32_t reloff;
	uint32_t nreloc;
	uint32_t flags;
	uint32_t reserved1;
	uint32_t reserved2;
	uint32_t reserved3;
};

struct symtab_command {
	uint32_t cmd;
	uint32_t cmdsize;
	uint32_t symoff;
	uint32_t nsyms;
	uint32_t stroff;
	uint32_t strsize;
};

struct note_command {
	uint32_t cmd;
	uint32_t cmdsize;
	char data_owner[16];
	uint64_t offset;
	uint64_t size;
};

struct encryption_info_command_64 {
	uint32_t cmd;
	uint32_t cmdsize;
	uint32_t cryptoff;
	uint32_t cryptsize;
	uint32_t cryptid;
	uint32_t pad;
};

struct nlist_64 {
	uint32_t n_strx;
	uint8_t n_type;
	uint8_t n_sect;
	uint16_t n_desc;
	uint64_t n_value;
};

#pragma pack(pop)

constexpr uint32_t MH_MAGIC_64 = 0xfeedfacf;
constexpr uint32_t MH_CIGAM_64 = 0xcffaedfe;
constexpr uint32_t FAT_MAGIC = 0xcafebabe;
constexpr uint32_t FAT_MAGIC_64 = 0xcafebabf;
constexpr int32_t CPU_TYPE_X86_64 = 0x01000007;
constexpr int32_t CPU_TYPE_ARM64 = 0x0100000c;
constexpr uint32_t LC_SEGMENT_64 = 0x19;
constexpr uint32_t LC_SYMTAB = 0x2;
constexpr uint32_t LC_NOTE = 0x31;
constexpr uint32_t LC_ENCRYPTION_INFO_64 = 0x2c;

constexpr char SNAPSHOT_NOTE_OWNER[] = "__dart_app_snap";
constexpr char CUSTOM_SEGMENT[] = "__CUSTOM";
constexpr char CUSTOM_SECTION[] = "__dart_app_snap";

}

struct MachOSegment {
	uint64_t vmaddr;
	uint64_t vmsize;
	uint64_t fileoff;
	uint64_t filesize;
};

struct MachOImage {
	const uint8_t* fullFile;
	size_t fullSize;
	const uint8_t* image;
	size_t imageOffset;
	size_t imageSize;
};

#ifdef _WIN32
static MappedFile load_map_file(const char* path)
{
	HANDLE hFile = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		throw std::invalid_argument(std::format("Cannot open input file: {}", path));
	}

	LARGE_INTEGER fileSize;
	if (!GetFileSizeEx(hFile, &fileSize)) {
		CloseHandle(hFile);
		throw std::invalid_argument(std::format("Cannot read input file size: {}", path));
	}

	HANDLE hMapFile = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
	if (hMapFile == INVALID_HANDLE_VALUE) {
		CloseHandle(hFile);
		throw std::invalid_argument(std::format("Cannot map input file: {}", path));
	}

	void* mem = MapViewOfFile(hMapFile, FILE_MAP_COPY, 0, 0, 0);
	CloseHandle(hMapFile);
	CloseHandle(hFile);
	if (mem == nullptr) {
		throw std::invalid_argument(std::format("Cannot map input file view: {}", path));
	}
	return MappedFile{ static_cast<uint8_t*>(mem), static_cast<size_t>(fileSize.QuadPart) };
}
#else
static MappedFile load_map_file(const char* path)
{
	int fd = open(path, O_RDONLY);
	if (fd == -1) {
		throw std::invalid_argument(std::format("Cannot open input file: {}", path));
	}

	struct stat st;
	if (fstat(fd, &st) == -1) {
		close(fd);
		throw std::invalid_argument(std::format("Cannot read input file size: {}", path));
	}

	void* mem = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	close(fd);
	if (mem == MAP_FAILED) {
		throw std::invalid_argument(std::format("Cannot map input file: {}", path));
	}
	return MappedFile{ static_cast<uint8_t*>(mem), static_cast<size_t>(st.st_size) };
}
#endif

template<typename T>
static const T* checked_ptr(const uint8_t* data, size_t size, size_t offset, const char* what)
{
	if (offset > size || sizeof(T) > size - offset) {
		throw std::invalid_argument(std::format("Mach-O: truncated {}", what));
	}
	return reinterpret_cast<const T*>(data + offset);
}

static uint32_t read_be32(const uint8_t* p)
{
	return (static_cast<uint32_t>(p[0]) << 24) |
		(static_cast<uint32_t>(p[1]) << 16) |
		(static_cast<uint32_t>(p[2]) << 8) |
		static_cast<uint32_t>(p[3]);
}

static uint64_t read_be64(const uint8_t* p)
{
	uint64_t result = 0;
	for (int i = 0; i < 8; i++) {
		result = (result << 8) | p[i];
	}
	return result;
}

static int32_t expected_macho_cpu_type()
{
#if defined(TARGET_ARCH_X64)
	return macho::CPU_TYPE_X86_64;
#else
	return macho::CPU_TYPE_ARM64;
#endif
}

static bool fixed_name_equals(const char* fixed, const char* expected)
{
	const size_t expectedLen = strlen(expected);
	return strncmp(fixed, expected, 16) == 0 && (expectedLen == 16 || fixed[expectedLen] == '\0');
}

static MachOImage select_macho_image(const uint8_t* file, size_t fileSize)
{
	if (fileSize < sizeof(uint32_t)) {
		throw std::invalid_argument("Mach-O: truncated header");
	}

	const uint32_t beMagic = read_be32(file);
	if (beMagic == macho::FAT_MAGIC || beMagic == macho::FAT_MAGIC_64) {
		const uint32_t nfat = read_be32(file + 4);
		size_t offset = sizeof(macho::fat_header);
		const auto expectedCpu = expected_macho_cpu_type();
		const uint8_t* selectedImage = nullptr;
		size_t selectedOffset = 0;
		size_t selectedSize = 0;

		for (uint32_t i = 0; i < nfat; i++) {
			int32_t cputype;
			uint64_t imageOffset;
			uint64_t imageSize;
			if (beMagic == macho::FAT_MAGIC) {
				const auto* arch = checked_ptr<macho::fat_arch>(file, fileSize, offset, "fat architecture");
				cputype = static_cast<int32_t>(read_be32(reinterpret_cast<const uint8_t*>(&arch->cputype)));
				imageOffset = read_be32(reinterpret_cast<const uint8_t*>(&arch->offset));
				imageSize = read_be32(reinterpret_cast<const uint8_t*>(&arch->size));
				offset += sizeof(macho::fat_arch);
			}
			else {
				const auto* arch = checked_ptr<macho::fat_arch_64>(file, fileSize, offset, "fat64 architecture");
				cputype = static_cast<int32_t>(read_be32(reinterpret_cast<const uint8_t*>(&arch->cputype)));
				imageOffset = read_be64(reinterpret_cast<const uint8_t*>(&arch->offset));
				imageSize = read_be64(reinterpret_cast<const uint8_t*>(&arch->size));
				offset += sizeof(macho::fat_arch_64);
			}

			if (imageOffset > fileSize || imageSize > fileSize - imageOffset) {
				throw std::invalid_argument("Mach-O: fat slice points outside file");
			}

			if (cputype == expectedCpu) {
				selectedImage = file + imageOffset;
				selectedOffset = static_cast<size_t>(imageOffset);
				selectedSize = static_cast<size_t>(imageSize);
				break;
			}
		}

		if (selectedImage == nullptr) {
			throw std::invalid_argument("Mach-O: cannot select a fat slice matching the selected Dart VM");
		}
		return MachOImage{ file, fileSize, selectedImage, selectedOffset, selectedSize };
	}

	return MachOImage{ file, fileSize, file, 0, fileSize };
}

static const uint8_t* macho_file_offset_ptr(const MachOImage& image, uint64_t fileoff, uint64_t size)
{
	if (fileoff > image.imageSize || size > image.imageSize - fileoff) {
		throw std::invalid_argument("Mach-O: file offset points outside image");
	}
	return image.image + fileoff;
}

static const uint8_t* macho_vmaddr_ptr(const MachOImage& image, const std::vector<MachOSegment>& segments, uint64_t vmaddr)
{
	for (const auto& segment : segments) {
		if (vmaddr >= segment.vmaddr && vmaddr < segment.vmaddr + segment.filesize) {
			const uint64_t fileoff = segment.fileoff + vmaddr - segment.vmaddr;
			return macho_file_offset_ptr(image, fileoff, 1);
		}
	}
	throw std::invalid_argument(std::format("Mach-O: VM address {:#x} is outside file-backed segments", vmaddr));
}

static const uint8_t* macho_note_ptr(const MachOImage& image, uint64_t offset, uint64_t size)
{
	if (offset <= image.imageSize && size <= image.imageSize - offset) {
		return image.image + offset;
	}
	if (offset <= image.fullSize && size <= image.fullSize - offset) {
		return image.fullFile + offset;
	}
	throw std::invalid_argument("Mach-O: snapshot note points outside file");
}

static LibAppInfo find_macho_snapshots(const MachOImage& image);

static LibAppInfo find_macho_snapshots_at(const MachOImage& owner, const uint8_t* nested, uint64_t nestedSize)
{
	if (nested < owner.fullFile || nested > owner.fullFile + owner.fullSize) {
		throw std::invalid_argument("Mach-O: nested snapshot points outside file");
	}
	const auto nestedOffset = static_cast<size_t>(nested - owner.fullFile);
	if (nestedSize > owner.fullSize - nestedOffset) {
		throw std::invalid_argument("Mach-O: nested snapshot points outside file");
	}
	return find_macho_snapshots(MachOImage{ owner.fullFile, owner.fullSize, nested, nestedOffset, static_cast<size_t>(nestedSize) });
}

static LibAppInfo find_macho_snapshots(const MachOImage& image)
{
	const auto* hdr = checked_ptr<macho::mach_header_64>(image.image, image.imageSize, 0, "header");
	if (hdr->magic == macho::MH_CIGAM_64) {
		throw std::invalid_argument("Mach-O: expected a host-endian 64-bit header");
	}
	if (hdr->magic != macho::MH_MAGIC_64) {
		throw std::invalid_argument("Mach-O: invalid 64-bit magic header");
	}
	if (hdr->cputype != expected_macho_cpu_type()) {
		throw std::invalid_argument("Mach-O: architecture does not match the selected Dart VM");
	}

	std::vector<MachOSegment> segments;
	const macho::symtab_command* symtab = nullptr;
	const macho::note_command* snapshotNote = nullptr;
	const macho::section_64* customSnapshotSection = nullptr;

	size_t commandOffset = sizeof(macho::mach_header_64);
	for (uint32_t i = 0; i < hdr->ncmds; i++) {
		const auto* command = checked_ptr<macho::load_command>(image.image, image.imageSize, commandOffset, "load command");
		if (command->cmdsize < sizeof(macho::load_command) || command->cmdsize > image.imageSize - commandOffset) {
			throw std::invalid_argument("Mach-O: invalid load command size");
		}

		if (command->cmd == macho::LC_SEGMENT_64) {
			const auto* segment = checked_ptr<macho::segment_command_64>(image.image, image.imageSize, commandOffset, "segment command");
			segments.push_back(MachOSegment{ segment->vmaddr, segment->vmsize, segment->fileoff, segment->filesize });

			const size_t sectionOffset = commandOffset + sizeof(macho::segment_command_64);
			for (uint32_t s = 0; s < segment->nsects; s++) {
				const auto* section = checked_ptr<macho::section_64>(image.image, image.imageSize, sectionOffset + s * sizeof(macho::section_64), "section");
				if (fixed_name_equals(section->segname, macho::CUSTOM_SEGMENT) && fixed_name_equals(section->sectname, macho::CUSTOM_SECTION)) {
					customSnapshotSection = section;
				}
			}
		}
		else if (command->cmd == macho::LC_SYMTAB) {
			symtab = checked_ptr<macho::symtab_command>(image.image, image.imageSize, commandOffset, "symbol table command");
		}
		else if (command->cmd == macho::LC_NOTE) {
			const auto* note = checked_ptr<macho::note_command>(image.image, image.imageSize, commandOffset, "note command");
			if (fixed_name_equals(note->data_owner, macho::SNAPSHOT_NOTE_OWNER)) {
				snapshotNote = note;
			}
		}
		else if (command->cmd == macho::LC_ENCRYPTION_INFO_64) {
			const auto* encryption = checked_ptr<macho::encryption_info_command_64>(image.image, image.imageSize, commandOffset, "encryption info command");
			if (encryption->cryptid != 0) {
				throw std::invalid_argument("Mach-O: image is encrypted. Use a decrypted iOS binary.");
			}
		}

		commandOffset += command->cmdsize;
	}

	if (symtab != nullptr) {
		const char* strtab = reinterpret_cast<const char*>(macho_file_offset_ptr(image, symtab->stroff, symtab->strsize));
		const auto* symbols = reinterpret_cast<const macho::nlist_64*>(macho_file_offset_ptr(image, symtab->symoff, static_cast<uint64_t>(symtab->nsyms) * sizeof(macho::nlist_64)));

		const uint8_t* vm_snapshot_data = nullptr;
		const uint8_t* vm_snapshot_instructions = nullptr;
		const uint8_t* isolate_snapshot_data = nullptr;
		const uint8_t* isolate_snapshot_instructions = nullptr;

		for (uint32_t i = 0; i < symtab->nsyms; i++) {
			const auto& sym = symbols[i];
			if (sym.n_strx == 0 || sym.n_strx >= symtab->strsize)
				continue;

			const char* name = strtab + sym.n_strx;
			if (strcmp(name, kVmSnapshotDataAsmSymbol) == 0 || strcmp(name, kVmSnapshotDataCSymbol) == 0) {
				vm_snapshot_data = macho_vmaddr_ptr(image, segments, sym.n_value);
			}
			else if (strcmp(name, kVmSnapshotInstructionsAsmSymbol) == 0 || strcmp(name, kVmSnapshotInstructionsCSymbol) == 0) {
				vm_snapshot_instructions = macho_vmaddr_ptr(image, segments, sym.n_value);
			}
			else if (strcmp(name, kIsolateSnapshotDataAsmSymbol) == 0 || strcmp(name, kIsolateSnapshotDataCSymbol) == 0) {
				isolate_snapshot_data = macho_vmaddr_ptr(image, segments, sym.n_value);
			}
			else if (strcmp(name, kIsolateSnapshotInstructionsAsmSymbol) == 0 || strcmp(name, kIsolateSnapshotInstructionsCSymbol) == 0) {
				isolate_snapshot_instructions = macho_vmaddr_ptr(image, segments, sym.n_value);
			}
		}

		if (vm_snapshot_data != nullptr && vm_snapshot_instructions != nullptr &&
			isolate_snapshot_data != nullptr && isolate_snapshot_instructions != nullptr) {
			return LibAppInfo{
				.lib = image.image,
				.vm_snapshot_data = vm_snapshot_data,
				.vm_snapshot_instructions = vm_snapshot_instructions,
				.isolate_snapshot_data = isolate_snapshot_data,
				.isolate_snapshot_instructions = isolate_snapshot_instructions,
			};
		}
	}

	if (snapshotNote != nullptr) {
		return find_macho_snapshots_at(image, macho_note_ptr(image, snapshotNote->offset, snapshotNote->size), snapshotNote->size);
	}

	if (customSnapshotSection != nullptr) {
		const auto* nested = macho_file_offset_ptr(image, customSnapshotSection->offset, customSnapshotSection->size);
		return find_macho_snapshots_at(image, nested, customSnapshotSection->size);
	}

	throw std::invalid_argument("Mach-O: cannot find Dart snapshots");
}

LibAppInfo ElfHelper::findElfSnapshots(const uint8_t* elf)
{
	const auto* hdr = (const ElfHeader*)elf;
	if (hdr->section_table_entry_size != sizeof(SectionHeader))
		throw std::invalid_argument("ELF: Invalid section entry size");

	const auto* section = (SectionHeader*)(elf + hdr->section_table_offset);
	const auto sh_num = hdr->num_section_headers;

	const char* dynstr = nullptr;
	const Symbol* dynsym = nullptr;
	const Symbol* dynsym_end = nullptr;
	for (uint16_t i = 0; i < sh_num; i++, section++) {
		if (section->type == SectionHeaderType::SHT_STRTAB && dynstr == nullptr) {
			const char* strtab = (const char*)elf + section->file_offset;
			const char* last = strtab + section->file_size;
			const char* s_first = kVmSnapshotDataAsmSymbol;
			const char* s_last = s_first + strlen(kVmSnapshotDataAsmSymbol) + 1;
			if (std::search(strtab, last, s_first, s_last) != last) {
				dynstr = strtab;
			}
		}
		if (section->type == SectionHeaderType::SHT_DYNSYM) {
			if (section->entry_size != sizeof(Symbol))
				throw std::invalid_argument("ELF: Invalid DYNSYM entry size");
			dynsym = (Symbol*)(elf + section->file_offset);
			dynsym_end = (Symbol*)(elf + section->file_offset + section->file_size);
		}
		if (dynsym != nullptr && dynstr != nullptr)
			break;
	}

	const uint8_t* vm_snapshot_data = nullptr;
	const uint8_t* vm_snapshot_instructions = nullptr;
	const uint8_t* isolate_snapshot_data = nullptr;
	const uint8_t* isolate_snapshot_instructions = nullptr;
	if (dynsym == nullptr || dynstr == nullptr) {
		throw std::invalid_argument("ELF: Cannot find dynamic symbols");
	}
	for (; dynsym < dynsym_end; dynsym++) {
		if (dynsym->info == 0)
			continue;

		const char* name = dynstr + dynsym->name;
		if (strcmp(name, kVmSnapshotDataAsmSymbol) == 0) {
			vm_snapshot_data = elf + dynsym->value;
		}
		else if (strcmp(name, kVmSnapshotInstructionsAsmSymbol) == 0) {
			vm_snapshot_instructions = elf + dynsym->value;
		}
		else if (strcmp(name, kIsolateSnapshotDataAsmSymbol) == 0) {
			isolate_snapshot_data = elf + dynsym->value;
		}
		else if (strcmp(name, kIsolateSnapshotInstructionsAsmSymbol) == 0) {
			isolate_snapshot_instructions = elf + dynsym->value;
		}
	}

	if (vm_snapshot_data == nullptr)
		throw std::invalid_argument("ELF: Cannot find Dart VM Snapshot Data");
	if (vm_snapshot_instructions == nullptr)
		throw std::invalid_argument("ELF: Cannot find Dart VM Snapshot Instructions");
	if (isolate_snapshot_data == nullptr)
		throw std::invalid_argument("ELF: Cannot find Dart Isolate Snapshot Data");
	if (isolate_snapshot_instructions == nullptr)
		throw std::invalid_argument("ELF: Cannot find Dart Isolate Snapshot Instructions");

	return LibAppInfo{
		.lib = elf,
		.vm_snapshot_data = vm_snapshot_data,
		.vm_snapshot_instructions = vm_snapshot_instructions,
		.isolate_snapshot_data = isolate_snapshot_data,
		.isolate_snapshot_instructions = isolate_snapshot_instructions,
	};
}

LibAppInfo ElfHelper::MapLibApp(const char* path)
{
	const auto mapped = load_map_file(path);
	const uint8_t* data = mapped.data;
	if (mapped.size < sizeof(uint32_t)) {
		throw std::invalid_argument("Input file is truncated");
	}

	const uint32_t beMagic = read_be32(data);
	if (beMagic == macho::FAT_MAGIC || beMagic == macho::FAT_MAGIC_64) {
		return find_macho_snapshots(select_macho_image(data, mapped.size));
	}

	const uint32_t leMagic = *reinterpret_cast<const uint32_t*>(data);
	if (leMagic == macho::MH_MAGIC_64 || leMagic == macho::MH_CIGAM_64) {
		return find_macho_snapshots(select_macho_image(data, mapped.size));
	}

	const auto* hdr = (ElfHeader*)data;
	const auto* ident = (ElfIdent*)hdr->ident;
	if (memcmp(ident->ei_magic, "\x7f" "ELF", 4) != 0)
		throw std::invalid_argument("Input file is neither ELF nor Mach-O");
	if (ident->ei_data != 1)
		throw std::invalid_argument("ELF: Support only little endian");

	if (ident->ei_class != ELFCLASS64) {
		throw std::invalid_argument("ELF: Support only 64 bits");
	}

	return findElfSnapshots(data);
}
