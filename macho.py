import re
import struct
from dataclasses import dataclass


MH_MAGIC_64 = 0xfeedfacf
MH_CIGAM_64 = 0xcffaedfe
FAT_MAGIC = 0xcafebabe
FAT_MAGIC_64 = 0xcafebabf

CPU_TYPE_X86_64 = 0x01000007
CPU_TYPE_ARM64 = 0x0100000C

LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
LC_NOTE = 0x31
LC_ENCRYPTION_INFO_64 = 0x2C

DART_SNAPSHOT_NOTE = "__dart_app_snap"
DART_CUSTOM_SEGMENT = "__CUSTOM"
DART_CUSTOM_SECTION = "__dart_app_snap"

DART_VM_SNAPSHOT_DATA_SYMBOLS = (
    "_kDartVmSnapshotData",
    "kDartVmSnapshotData",
)


@dataclass(frozen=True)
class FatArch:
    cputype: int
    offset: int
    size: int


@dataclass(frozen=True)
class Section:
    sectname: str
    segname: str
    addr: int
    size: int
    offset: int


@dataclass(frozen=True)
class Segment:
    name: str
    vmaddr: int
    vmsize: int
    fileoff: int
    filesize: int
    sections: tuple[Section, ...]


@dataclass(frozen=True)
class Symtab:
    symoff: int
    nsyms: int
    stroff: int
    strsize: int


@dataclass(frozen=True)
class Note:
    owner: str
    offset: int
    size: int


@dataclass(frozen=True)
class EncryptionInfo:
    cryptoff: int
    cryptsize: int
    cryptid: int


def _read_cstr(data: bytes, offset: int, limit: int) -> str:
    end = data.find(b"\0", offset, limit)
    if end == -1:
        end = limit
    return data[offset:end].decode("ascii")


def _read_fixed_name(raw: bytes) -> str:
    end = raw.find(b"\0")
    if end == -1:
        end = len(raw)
    return raw[:end].decode("ascii")


def _u32be(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def _parse_fat_arches(data: bytes) -> list[FatArch]:
    magic = _u32be(data, 0)
    if magic not in (FAT_MAGIC, FAT_MAGIC_64):
        return []

    nfat_arch = _u32be(data, 4)
    arches = []
    offset = 8
    for _ in range(nfat_arch):
        if magic == FAT_MAGIC:
            cputype, _, arch_offset, arch_size, _ = struct.unpack_from(">IIIII", data, offset)
            offset += 20
        else:
            cputype, _, arch_offset, arch_size, _, _ = struct.unpack_from(">IIQQII", data, offset)
            offset += 32
        arches.append(FatArch(cputype, arch_offset, arch_size))
    return arches


def _select_fat_arch(arches: list[FatArch], preferred_arch: str | None) -> FatArch:
    preferred_cpu = {
        None: CPU_TYPE_ARM64,
        "arm64": CPU_TYPE_ARM64,
        "x64": CPU_TYPE_X86_64,
    }[preferred_arch]

    for arch in arches:
        if arch.cputype == preferred_cpu:
            return arch

    assert preferred_arch is None, f"Mach-O fat binary has no {preferred_arch} slice"

    for arch in arches:
        if arch.cputype in (CPU_TYPE_ARM64, CPU_TYPE_X86_64):
            return arch

    assert False, "Mach-O fat binary has no supported arm64 or x64 slice"


class MachOImage:
    def __init__(self, data: bytes, offset: int = 0, size: int | None = None):
        self.data = data
        self.offset = offset
        self.size = len(data) - offset if size is None else size

        magic = struct.unpack_from("<I", data, offset)[0]
        assert magic != MH_CIGAM_64, "Mach-O big-endian 64-bit images are unsupported"
        assert magic == MH_MAGIC_64, "Mach-O image must be 64-bit little-endian"

        _, self.cputype, _, _, self.ncmds, self.sizeofcmds, _flags, _reserved = struct.unpack_from("<IiiIIIII", data, offset)
        self.load_commands_offset = offset + 32
        self.segments: list[Segment] = []
        self.symtab: Symtab | None = None
        self.notes: list[Note] = []
        self.encryption_info: EncryptionInfo | None = None
        self._parse_load_commands()

    @property
    def arch(self) -> str:
        if self.cputype == CPU_TYPE_ARM64:
            return "arm64"
        if self.cputype == CPU_TYPE_X86_64:
            return "x64"
        assert False, f"Unsupported Mach-O architecture: {self.cputype:#x}"

    @property
    def is_encrypted(self) -> bool:
        return self.encryption_info is not None and self.encryption_info.cryptid != 0

    def _parse_load_commands(self):
        command_offset = self.load_commands_offset
        for _ in range(self.ncmds):
            cmd, cmdsize = struct.unpack_from("<II", self.data, command_offset)
            assert cmdsize >= 8, "Mach-O load command is truncated"

            if cmd == LC_SEGMENT_64:
                fields = struct.unpack_from("<II16sQQQQiiII", self.data, command_offset)
                _, _, segname_raw, vmaddr, vmsize, fileoff, filesize, _, _, nsects, _ = fields
                section_offset = command_offset + 72
                sections = []
                for _ in range(nsects):
                    sect = struct.unpack_from("<16s16sQQIIIIIIII", self.data, section_offset)
                    sectname, segname, addr, size, offset = sect[:5]
                    sections.append(Section(_read_fixed_name(sectname), _read_fixed_name(segname), addr, size, offset))
                    section_offset += 80
                self.segments.append(Segment(_read_fixed_name(segname_raw), vmaddr, vmsize, fileoff, filesize, tuple(sections)))
            elif cmd == LC_SYMTAB:
                _, _, symoff, nsyms, stroff, strsize = struct.unpack_from("<IIIIII", self.data, command_offset)
                self.symtab = Symtab(symoff, nsyms, stroff, strsize)
            elif cmd == LC_NOTE:
                _, _, owner_raw, note_offset, size = struct.unpack_from("<II16sQQ", self.data, command_offset)
                self.notes.append(Note(_read_fixed_name(owner_raw), note_offset, size))
            elif cmd == LC_ENCRYPTION_INFO_64:
                _, _, cryptoff, cryptsize, cryptid, _pad = struct.unpack_from("<IIIIII", self.data, command_offset)
                self.encryption_info = EncryptionInfo(cryptoff, cryptsize, cryptid)

            command_offset += cmdsize

    def vmaddr_to_fileoff(self, vmaddr: int) -> int:
        for segment in self.segments:
            if segment.vmaddr <= vmaddr < segment.vmaddr + segment.filesize:
                return segment.fileoff + vmaddr - segment.vmaddr
        assert False, f"Mach-O VM address is outside file-backed segments: {vmaddr:#x}"

    def data_at_vmaddr(self, vmaddr: int, size: int) -> bytes:
        fileoff = self.vmaddr_to_fileoff(vmaddr)
        start = self.offset + fileoff
        end = start + size
        assert end <= self.offset + self.size, "Mach-O VM address points outside image"
        return self.data[start:end]

    def data_at_fileoff(self, fileoff: int, size: int) -> bytes:
        start = self.offset + fileoff
        end = start + size
        assert end <= self.offset + self.size, "Mach-O file offset points outside image"
        return self.data[start:end]

    def iter_symbols(self):
        if self.symtab is None:
            return

        str_start = self.offset + self.symtab.stroff
        str_limit = str_start + self.symtab.strsize
        symbol_offset = self.offset + self.symtab.symoff
        for i in range(self.symtab.nsyms):
            entry_offset = symbol_offset + i * 16
            n_strx, _type, _sect, _desc, n_value = struct.unpack_from("<IBBHQ", self.data, entry_offset)
            if n_strx == 0:
                continue
            name = _read_cstr(self.data, str_start + n_strx, str_limit)
            yield name, n_value

    def symbol_data(self, names: tuple[str, ...], size: int) -> bytes | None:
        for name, vmaddr in self.iter_symbols():
            if name in names:
                return self.data_at_vmaddr(vmaddr, size)
        return None

    def find_section(self, segname: str, sectname: str) -> Section | None:
        for segment in self.segments:
            if segment.name != segname:
                continue
            for section in segment.sections:
                if section.sectname == sectname:
                    return section
        return None


def load_macho_image(path: str, preferred_arch: str | None = None) -> MachOImage:
    with open(path, "rb") as f:
        data = f.read()

    arches = _parse_fat_arches(data)
    if arches:
        arch = _select_fat_arch(arches, preferred_arch)
        return MachOImage(data, arch.offset, arch.size)

    return MachOImage(data)


def is_macho_file(path: str) -> bool:
    with open(path, "rb") as f:
        data = f.read(8)
    if len(data) < 4:
        return False
    magic_le = struct.unpack_from("<I", data, 0)[0]
    magic_be = struct.unpack_from(">I", data, 0)[0]
    return magic_le in (MH_MAGIC_64, MH_CIGAM_64) or magic_be in (FAT_MAGIC, FAT_MAGIC_64)


def _nested_image_offset(image: MachOImage, offset: int) -> int:
    image_relative = image.offset + offset
    if image_relative + 4 <= image.offset + image.size:
        return image_relative
    if offset + 4 <= len(image.data):
        return offset
    return image_relative


def find_dart_vm_snapshot_data(image: MachOImage, size: int = 512) -> bytes:
    data = image.symbol_data(DART_VM_SNAPSHOT_DATA_SYMBOLS, size)
    if data is not None:
        return data

    for note in image.notes:
        if note.owner == DART_SNAPSHOT_NOTE:
            nested = MachOImage(image.data, _nested_image_offset(image, note.offset), note.size)
            return find_dart_vm_snapshot_data(nested, size)

    section = image.find_section(DART_CUSTOM_SEGMENT, DART_CUSTOM_SECTION)
    if section is not None:
        nested = MachOImage(image.data, image.offset + section.offset, section.size)
        return find_dart_vm_snapshot_data(nested, size)

    assert False, "Cannot find Dart VM snapshot data in Mach-O image"


def find_engine_ids(data: bytes) -> list[str]:
    hashes = re.findall(rb"\x00([a-f\d]{40})(?=\x00)", data)
    seen = set()
    engine_ids = []
    for item in hashes:
        value = item.decode()
        if value not in seen:
            seen.add(value)
            engine_ids.append(value)
    return engine_ids


def find_dart_version(data: bytes) -> str | None:
    match = re.search(rb"\x00([\d\w\.-]+) \((stable|beta|dev)\)", data)
    if match is None:
        return None
    return match.group(1).decode()
