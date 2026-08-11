"""Universal (fat) Mach-O slice selection, in both readers.

Issue #5: `MachOHelper::MapLibApp` (C++) and `MachO._find_slice` (Python) both
parse a fat header and prefer the arm64 slice, and neither had ever run against
a universal binary.

The ordering cases matter more than they look. A real universal build — checked
against AppFlowy 0.13.1 macos-universal — lists **x86_64 first** and arm64
second, so an implementation that quietly takes the first slice picks the wrong
architecture and fails later, somewhere unrelated.

Fixtures are synthesized (see fat_macho.py) rather than committed: a real
universal binary is hundreds of megabytes of third-party software. The
integration test below wraps a *real* thin image supplied via the environment,
so the synthetic path is checked against genuine Mach-O content at least once.

The unit tests need `extract_dart_info`, which imports requests and pyelftools;
run them with the interpreter Blutter itself uses.
"""

import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from fat_macho import (  # noqa: E402
    CPU_TYPE_ARM64,
    CPU_TYPE_X86_64,
    build_fat,
    build_fat64_header,
    thin_macho64,
)

try:
    from extract_dart_info import MachO, is_macho

    IMPORT_ERROR = None
except ImportError as exc:  # pragma: no cover - depends on the environment
    MachO = None
    is_macho = None
    IMPORT_ERROR = str(exc)

CPU_TYPE_PPC = 0x00000012  # something neither reader supports


def write(tmpdir: str, name: str, data: bytes) -> str:
    path = os.path.join(tmpdir, name)
    with open(path, "wb") as f:
        f.write(data)
    return path


@unittest.skipIf(MachO is None, f"extract_dart_info not importable: {IMPORT_ERROR}")
class FindSliceTests(unittest.TestCase):
    """The Python reader."""

    def test_thin_image_has_no_slice_offset(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = write(tmp, "thin", thin_macho64(CPU_TYPE_ARM64))
            self.assertEqual(MachO(path).slice_off, 0)

    def test_prefers_arm64_when_x86_64_is_listed_first(self):
        """The layout a real universal binary actually uses."""
        with tempfile.TemporaryDirectory() as tmp:
            fat = build_fat(
                [
                    (CPU_TYPE_X86_64, thin_macho64(CPU_TYPE_X86_64, b"\x11" * 64)),
                    (CPU_TYPE_ARM64, thin_macho64(CPU_TYPE_ARM64, b"\x22" * 64)),
                ]
            )
            path = write(tmp, "fat", fat)
            macho = MachO(path)
            self.assertGreater(macho.slice_off, 0)
            self.assertEqual(macho.data[macho.slice_off + 4 : macho.slice_off + 8][:1], b"\x0c")

    def test_prefers_arm64_when_it_is_listed_first(self):
        with tempfile.TemporaryDirectory() as tmp:
            fat = build_fat(
                [
                    (CPU_TYPE_ARM64, thin_macho64(CPU_TYPE_ARM64, b"\x22" * 64)),
                    (CPU_TYPE_X86_64, thin_macho64(CPU_TYPE_X86_64, b"\x11" * 64)),
                ]
            )
            path = write(tmp, "fat", fat)
            self.assertEqual(MachO(path).slice_off, 1 << 14)

    def test_falls_back_to_x86_64(self):
        with tempfile.TemporaryDirectory() as tmp:
            fat = build_fat([(CPU_TYPE_X86_64, thin_macho64(CPU_TYPE_X86_64, b"\x11" * 64))])
            path = write(tmp, "fat", fat)
            self.assertEqual(MachO(path).slice_off, 1 << 14)

    def test_rejects_a_universal_binary_with_no_supported_slice(self):
        with tempfile.TemporaryDirectory() as tmp:
            fat = build_fat([(CPU_TYPE_PPC, thin_macho64(CPU_TYPE_PPC, b"\x33" * 64))])
            path = write(tmp, "fat", fat)
            with self.assertRaises(AssertionError):
                MachO(path)

    def test_is_macho_recognises_a_fat_image(self):
        with tempfile.TemporaryDirectory() as tmp:
            fat = build_fat([(CPU_TYPE_ARM64, thin_macho64(CPU_TYPE_ARM64))])
            self.assertTrue(is_macho(write(tmp, "fat", fat)))

    def test_fat_magic_64_fails_loudly_rather_than_misparsing(self):
        """FAT_MAGIC_64 uses 32-byte entries; misreading them silently is worse
        than refusing the file."""
        with tempfile.TemporaryDirectory() as tmp:
            data = build_fat64_header([(CPU_TYPE_ARM64, thin_macho64(CPU_TYPE_ARM64))])
            path = write(tmp, "fat64", data)
            with self.assertRaises(AssertionError):
                MachO(path)


def version_string(arch: str, version: str = "3.11.5") -> bytes:
    """The engine's embedded Dart version string, which names its target."""
    return (
        f'{version} (stable) (Mon Jan 1 00:00:00 2026 +0000) on "macos_{arch}"'.encode()
    )


@unittest.skipIf(MachO is None, f"extract_dart_info not importable: {IMPORT_ERROR}")
class VersionStringSliceTests(unittest.TestCase):
    """Reading the target out of a universal engine binary.

    Every slice of a universal build carries its own version string. Searching
    the whole file finds whichever slice comes first, which is how a universal
    macOS engine reported `x64` while the snapshot it shipped alongside said
    `arm64` — the analysis would then have been attempted with the wrong Dart
    VM entirely.
    """

    def build_universal_engine(self, tmp: str, first: str, second: str) -> str:
        from fat_macho import build_fat as _build_fat

        cpu = {"x64": CPU_TYPE_X86_64, "arm64": CPU_TYPE_ARM64}
        fat = _build_fat(
            [
                (cpu[first], thin_macho64(cpu[first], version_string(first))),
                (cpu[second], thin_macho64(cpu[second], version_string(second))),
            ]
        )
        return write(tmp, "FlutterMacOS", fat)

    def test_arch_comes_from_the_selected_slice_not_the_first_one(self):
        from extract_dart_info import extract_flutter_framework_info

        with tempfile.TemporaryDirectory() as tmp:
            path = self.build_universal_engine(tmp, first="x64", second="arm64")
            _, version, arch, os_name = extract_flutter_framework_info(path)
            self.assertEqual((version, arch, os_name), ("3.11.5", "arm64", "macos"))

    def test_arch_is_right_when_arm64_is_listed_first_too(self):
        from extract_dart_info import extract_flutter_framework_info

        with tempfile.TemporaryDirectory() as tmp:
            path = self.build_universal_engine(tmp, first="arm64", second="x64")
            _, version, arch, os_name = extract_flutter_framework_info(path)
            self.assertEqual(arch, "arm64")

    def test_thin_engine_still_reports_its_own_arch(self):
        from extract_dart_info import extract_flutter_framework_info

        with tempfile.TemporaryDirectory() as tmp:
            path = write(
                tmp, "FlutterMacOS", thin_macho64(CPU_TYPE_ARM64, version_string("arm64"))
            )
            _, _, arch, os_name = extract_flutter_framework_info(path)
            self.assertEqual((arch, os_name), ("arm64", "macos"))


BLUTTER_BIN = os.environ.get("BLUTTER_BIN")
THIN_SAMPLE = os.environ.get("BLUTTER_TEST_MACHO")


@unittest.skipUnless(
    BLUTTER_BIN
    and pathlib.Path(BLUTTER_BIN).is_file()
    and THIN_SAMPLE
    and pathlib.Path(THIN_SAMPLE).is_file(),
    "set BLUTTER_BIN and BLUTTER_TEST_MACHO (a thin arm64 App) for the loader test",
)
class LoaderTests(unittest.TestCase):
    """The C++ reader, exercised through the executable.

    Wrapping a real thin App in a fat container and analyzing it must produce
    exactly what analyzing the thin image produces — the container is the only
    difference.
    """

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        tmp = pathlib.Path(cls._tmp.name)

        thin = pathlib.Path(THIN_SAMPLE).read_bytes()
        # x86_64 first, matching how real universal binaries are laid out.
        fat = build_fat(
            [
                (CPU_TYPE_X86_64, thin_macho64(CPU_TYPE_X86_64, b"\x00" * 4096)),
                (CPU_TYPE_ARM64, thin),
            ]
        )
        cls.fat_path = tmp / "App_fat"
        cls.fat_path.write_bytes(fat)

        cls.thin_out = tmp / "thin_out"
        cls.fat_out = tmp / "fat_out"
        cls.thin_result = cls.run_blutter(THIN_SAMPLE, cls.thin_out)
        cls.fat_result = cls.run_blutter(str(cls.fat_path), cls.fat_out)

    @staticmethod
    def run_blutter(infile: str, outdir: pathlib.Path) -> subprocess.CompletedProcess:
        return subprocess.run(
            [BLUTTER_BIN, "-i", infile, "-o", str(outdir)],
            capture_output=True,
            text=True,
            timeout=1800,
        )

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def test_thin_baseline_succeeds(self):
        """If this fails the comparison below proves nothing."""
        self.assertEqual(self.thin_result.returncode, 0, self.thin_result.stderr[-2000:])

    def test_fat_image_is_analyzed(self):
        self.assertEqual(self.fat_result.returncode, 0, self.fat_result.stderr[-2000:])

    def test_fat_and_thin_produce_the_same_files(self):
        thin_files = {p.relative_to(self.thin_out) for p in self.thin_out.rglob("*") if p.is_file()}
        fat_files = {p.relative_to(self.fat_out) for p in self.fat_out.rglob("*") if p.is_file()}
        self.assertEqual(thin_files, fat_files)
        self.assertGreater(len(thin_files), 0, "no output at all")

    def test_fat_and_thin_disassembly_agree(self):
        """Compared after masking host addresses, which vary run to run."""
        import re

        host = re.compile(r"0x[0-9a-f]{10,}|\b\d{12,}\b|@[0-9a-f]{8}")
        differing = []
        for thin_file in sorted(self.thin_out.rglob("*.dart")):
            fat_file = self.fat_out / thin_file.relative_to(self.thin_out)
            a = host.sub("X", thin_file.read_text(errors="replace"))
            b = host.sub("X", fat_file.read_text(errors="replace"))
            if a != b:
                differing.append(str(thin_file.relative_to(self.thin_out)))
        self.assertEqual(differing[:5], [], f"{len(differing)} files differ")


if __name__ == "__main__":
    unittest.main()
