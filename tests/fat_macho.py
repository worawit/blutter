"""Build universal (fat) Mach-O images for tests, without needing macOS.

A fat binary is a big-endian header listing architecture slices, each slice a
complete thin Mach-O at some offset. Committing a real universal binary is not
an option — they are hundreds of megabytes of third-party software — so tests
synthesize one around whatever thin image they have.

Standard library only.
"""

import struct

FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF  # fat_arch_64 entries; not currently supported
MH_MAGIC_64 = 0xFEEDFACF

CPU_TYPE_X86_64 = 0x01000007
CPU_TYPE_ARM64 = 0x0100000C

MH_DYLIB = 0x6
DEFAULT_ALIGN = 14  # 2**14 = 16384, what Apple's tooling uses

LC_BUILD_VERSION = 0x32

PLATFORM_MACOS = 1
PLATFORM_IOS = 2


def build_version_command(platform: int) -> bytes:
    """An LC_BUILD_VERSION load command, which names the target platform.

    Both a real iOS and a real macOS Flutter engine carry one, so it is a more
    dependable source of the target than any embedded string.
    """
    return struct.pack(
        "<IIIIII",
        LC_BUILD_VERSION,
        24,  # cmdsize, no tool entries
        platform,
        0,  # minos
        0,  # sdk
        0,  # ntools
    )


def thin_macho64(
    cputype: int = CPU_TYPE_ARM64,
    body: bytes = b"",
    commands: "list[bytes] | None" = None,
) -> bytes:
    """A minimal but structurally valid 64-bit thin Mach-O.

    `commands` are whole load commands, already packed.
    """
    commands = list(commands or [])
    payload = b"".join(commands)
    header = struct.pack(
        "<IiiIIIII",
        MH_MAGIC_64,
        cputype,
        0,  # cpusubtype
        MH_DYLIB,  # filetype
        len(commands),  # ncmds
        len(payload),  # sizeofcmds
        0,  # flags
        0,  # reserved
    )
    return header + payload + body


def build_fat(slices: list[tuple[int, bytes]], align: int = DEFAULT_ALIGN) -> bytes:
    """Wrap `slices` — (cputype, thin image) pairs — in a fat container.

    Slice order is preserved, which is the point: a real universal binary may
    list x86_64 before arm64, and selection must not depend on position.
    """
    alignment = 1 << align
    header_size = 8 + 20 * len(slices)

    offsets = []
    cursor = (header_size + alignment - 1) // alignment * alignment
    for _, data in slices:
        offsets.append(cursor)
        cursor = (cursor + len(data) + alignment - 1) // alignment * alignment

    out = bytearray(struct.pack(">II", FAT_MAGIC, len(slices)))
    for (cputype, data), offset in zip(slices, offsets):
        out += struct.pack(">iIIII", cputype, 0, offset, len(data), align)

    for (_, data), offset in zip(slices, offsets):
        out += b"\x00" * (offset - len(out))
        out += data

    return bytes(out)


def build_fat64_header(slices: list[tuple[int, bytes]]) -> bytes:
    """A FAT_MAGIC_64 header, which uses 32-byte fat_arch_64 entries.

    Only the header is needed: the point is that the format is recognisably a
    universal binary that the current readers do not support, and that they say
    so rather than misparsing it.
    """
    out = bytearray(struct.pack(">II", FAT_MAGIC_64, len(slices)))
    offset = 8 + 32 * len(slices)
    for cputype, data in slices:
        out += struct.pack(">iIQQI", cputype, 0, offset, len(data), DEFAULT_ALIGN)
        out += b"\x00" * 4  # reserved
        offset += len(data)
    return bytes(out)
