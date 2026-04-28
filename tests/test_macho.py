import os
import struct
import tempfile
import unittest
import zipfile

from blutter import extract_libs_from_ipa
from extract_dart_info import extract_libflutter_info, extract_snapshot_hash_flags
from macho import CPU_TYPE_ARM64, FAT_MAGIC, find_dart_vm_snapshot_data, find_engine_ids, load_macho_image


BASE_VMADDR = 0x100000000
SNAPSHOT_OFFSET = 0x300
SNAPSHOT_HASH = b"0123456789abcdef0123456789abcdef"
SNAPSHOT_FLAGS = b"compressed-pointers null-safety"


def _fixed_name(value: bytes) -> bytes:
    return value + (b"\0" * (16 - len(value)))


def make_thin_macho() -> bytes:
    size = 0x500
    data = bytearray(size)
    string_table = b"\0_kDartVmSnapshotData\0"
    symoff = 0x180
    stroff = 0x200

    header = struct.pack("<IiiIIIII", 0xfeedfacf, CPU_TYPE_ARM64, 0, 6, 2, 96, 0, 0)
    segment = struct.pack(
        "<II16sQQQQiiII",
        0x19,
        72,
        _fixed_name(b"__TEXT"),
        BASE_VMADDR,
        size,
        0,
        size,
        7,
        5,
        0,
        0,
    )
    symtab = struct.pack("<IIIIII", 0x2, 24, symoff, 1, stroff, len(string_table))
    data[: len(header)] = header
    data[len(header): len(header) + len(segment)] = segment
    data[len(header) + len(segment): len(header) + len(segment) + len(symtab)] = symtab
    data[symoff:symoff + 16] = struct.pack("<IBBHQ", 1, 0x0F, 1, 0, BASE_VMADDR + SNAPSHOT_OFFSET)
    data[stroff:stroff + len(string_table)] = string_table
    data[SNAPSHOT_OFFSET + 20:SNAPSHOT_OFFSET + 52] = SNAPSHOT_HASH
    data[SNAPSHOT_OFFSET + 52:SNAPSHOT_OFFSET + 52 + len(SNAPSHOT_FLAGS)] = SNAPSHOT_FLAGS
    return bytes(data)


def write_temp_file(testcase: unittest.TestCase, data: bytes) -> str:
    fd, path = tempfile.mkstemp()
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    testcase.addCleanup(os.unlink, path)
    return path


class MachOTests(unittest.TestCase):
    def test_extracts_snapshot_data_from_thin_macho_symbol(self):
        path = write_temp_file(self, make_thin_macho())
        image = load_macho_image(path, "arm64")
        data = find_dart_vm_snapshot_data(image)

        self.assertEqual(SNAPSHOT_HASH, data[20:52])
        self.assertEqual(SNAPSHOT_FLAGS, data[52:52 + len(SNAPSHOT_FLAGS)])

    def test_selects_arm64_slice_from_fat_macho(self):
        thin = make_thin_macho()
        fat_offset = 0x100
        header = struct.pack(">II", FAT_MAGIC, 1)
        arch = struct.pack(">IIIII", CPU_TYPE_ARM64, 0, fat_offset, len(thin), 14)
        data = bytearray(fat_offset + len(thin))
        data[:len(header)] = header
        data[len(header):len(header) + len(arch)] = arch
        data[fat_offset:fat_offset + len(thin)] = thin
        path = write_temp_file(self, bytes(data))

        image = load_macho_image(path, "arm64")
        snapshot = find_dart_vm_snapshot_data(image)

        self.assertEqual("arm64", image.arch)
        self.assertEqual(SNAPSHOT_HASH, snapshot[20:52])

    def test_deduplicates_engine_ids(self):
        engine_id = b"0123456789abcdef0123456789abcdef01234567"
        data = b"\0" + engine_id + b"\0x\0" + engine_id + b"\0"

        self.assertEqual([engine_id.decode()], find_engine_ids(data))

    def test_extracts_snapshot_hash_and_flags_from_macho(self):
        path = write_temp_file(self, make_thin_macho())

        snapshot_hash, flags = extract_snapshot_hash_flags(path)

        self.assertEqual(SNAPSHOT_HASH.decode(), snapshot_hash)
        self.assertEqual(["compressed-pointers", "null-safety"], flags)

    def test_extracts_flutter_info_from_macho(self):
        engine_id = b"0123456789abcdef0123456789abcdef01234567"
        data = bytearray(make_thin_macho())
        data[0x380:0x380 + len(engine_id) + 2] = b"\0" + engine_id + b"\0"
        version = b"\0" + b"3.4.2 (stable)" + b"\0"
        data[0x3C0:0x3C0 + len(version)] = version
        path = write_temp_file(self, bytes(data))

        engine_ids, dart_version, arch, os_name = extract_libflutter_info(path)

        self.assertEqual([engine_id.decode()], engine_ids)
        self.assertEqual("3.4.2", dart_version)
        self.assertEqual("arm64", arch)
        self.assertEqual("ios", os_name)

    def test_extracts_ios_flutter_info_without_engine_id(self):
        data = bytearray(make_thin_macho())
        version = b"\0" + b"3.9.2 (stable) (Wed Aug 27 03:49:40 2025 -0700) on \"ios_arm64\"" + b"\0"
        data[0x380:0x380 + len(version)] = version
        path = write_temp_file(self, bytes(data))

        engine_ids, dart_version, arch, os_name = extract_libflutter_info(path)

        self.assertEqual([], engine_ids)
        self.assertEqual("3.9.2", dart_version)
        self.assertEqual("arm64", arch)
        self.assertEqual("ios", os_name)

    def test_extracts_ios_frameworks_from_ipa(self):
        with tempfile.TemporaryDirectory() as tmp:
            ipa_path = os.path.join(tmp, "app.ipa")
            with zipfile.ZipFile(ipa_path, "w") as zf:
                zf.writestr("Payload/Runner.app/Frameworks/App.framework/App", b"app")
                zf.writestr("Payload/Runner.app/Frameworks/Flutter.framework/Flutter", b"flutter")

            out_dir = os.path.join(tmp, "out")
            app_file, flutter_file = extract_libs_from_ipa(ipa_path, out_dir)

            with open(app_file, "rb") as f:
                self.assertEqual(b"app", f.read())
            with open(flutter_file, "rb") as f:
                self.assertEqual(b"flutter", f.read())
