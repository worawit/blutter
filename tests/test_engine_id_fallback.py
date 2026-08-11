"""Recovering the Dart version when the engine does not name it.

Issue #4: the ELF path handles an engine whose Dart version string is absent —
beta and dev channel builds do not always carry one — by collecting the engine
ids embedded alongside it and looking them up against the Flutter SDK archive.
The Mach-O path asserted instead, so such an app failed outright.

Two facts from real binaries shape this, both checked rather than assumed:

- A macOS engine embeds engine ids exactly as an ELF one does (AppFlowy 0.13.1
  carries two). An **iOS** engine embeds none — the only 40-hex runs in one are
  byte tables, not strings. So the lookup can rescue a macOS app and cannot
  rescue an iOS one, and the iOS case has to fail with something a reader can
  act on.
- Both carry `LC_BUILD_VERSION` naming the platform, and their header names the
  architecture. Reading those rather than the version string is what lets the
  target survive the string being missing at all.

Standard library only; imports extract_dart_info, so run with the interpreter
Blutter itself uses.
"""

import os
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from fat_macho import (  # noqa: E402
    CPU_TYPE_ARM64,
    CPU_TYPE_X86_64,
    PLATFORM_IOS,
    PLATFORM_MACOS,
    build_version_command,
    thin_macho64,
)

STRICT = os.environ.get("BLUTTER_TESTS_STRICT") == "1"

try:
    from extract_dart_info import extract_flutter_framework_info

    IMPORT_ERROR = None
except ImportError as exc:  # pragma: no cover - depends on the environment
    if STRICT:
        raise
    extract_flutter_framework_info = None
    IMPORT_ERROR = str(exc)

ENGINE_ID_A = "42d3d75a56ef1f1a3b0c9e8d7f6a5b4c3d2e1f00"
ENGINE_ID_B = "a183ded9ad6712345678901234567890abcdef12"


def engine_ids_blob(*ids: str) -> bytes:
    """Engine ids as the linker leaves them: NUL-delimited C strings."""
    return b"\x00" + b"\x00".join(i.encode() for i in ids) + b"\x00"


def version_blob(version: str = "3.10.7", target: str = "ios_arm64") -> bytes:
    return f'\x00{version} (stable) (Mon Jan 1 00:00:00 2026 +0000) on "{target}"\x00'.encode()


def write(tmp: str, data: bytes) -> str:
    path = os.path.join(tmp, "Flutter")
    with open(path, "wb") as f:
        f.write(data)
    return path


@unittest.skipIf(
    extract_flutter_framework_info is None, f"extract_dart_info not importable: {IMPORT_ERROR}"
)
class EngineFallbackTests(unittest.TestCase):
    def test_version_string_is_used_when_present(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = write(
                tmp,
                thin_macho64(
                    CPU_TYPE_ARM64,
                    version_blob("3.10.7", "ios_arm64"),
                    [build_version_command(PLATFORM_IOS)],
                ),
            )
            engine_ids, version, arch, os_name = extract_flutter_framework_info(path)
            self.assertEqual((version, arch, os_name), ("3.10.7", "arm64", "ios"))

    def test_engine_ids_are_collected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = write(
                tmp,
                thin_macho64(
                    CPU_TYPE_ARM64,
                    engine_ids_blob(ENGINE_ID_A, ENGINE_ID_B) + version_blob(),
                    [build_version_command(PLATFORM_IOS)],
                ),
            )
            engine_ids, _, _, _ = extract_flutter_framework_info(path)
            self.assertEqual(engine_ids, [ENGINE_ID_A, ENGINE_ID_B])

    def test_missing_version_string_is_not_fatal(self):
        """The beta/dev case. Returning None hands the caller the lookup."""
        with tempfile.TemporaryDirectory() as tmp:
            path = write(
                tmp,
                thin_macho64(
                    CPU_TYPE_ARM64,
                    engine_ids_blob(ENGINE_ID_A, ENGINE_ID_B),
                    [build_version_command(PLATFORM_MACOS)],
                ),
            )
            engine_ids, version, arch, os_name = extract_flutter_framework_info(path)
            self.assertIsNone(version)
            self.assertEqual(engine_ids, [ENGINE_ID_A, ENGINE_ID_B])
            self.assertEqual((arch, os_name), ("arm64", "macos"))

    def test_target_survives_a_missing_version_string(self):
        """Without the string there is nothing else to read the target from."""
        with tempfile.TemporaryDirectory() as tmp:
            path = write(
                tmp,
                thin_macho64(
                    CPU_TYPE_X86_64, b"", [build_version_command(PLATFORM_MACOS)]
                ),
            )
            _, version, arch, os_name = extract_flutter_framework_info(path)
            self.assertIsNone(version)
            self.assertEqual((arch, os_name), ("x64", "macos"))

    def test_header_wins_over_a_disagreeing_version_string(self):
        """The header describes the image; the string describes whatever build
        wrote it, and a universal binary carries one per slice."""
        with tempfile.TemporaryDirectory() as tmp:
            path = write(
                tmp,
                thin_macho64(
                    CPU_TYPE_ARM64,
                    version_blob("3.11.5", "macos_x64"),
                    [build_version_command(PLATFORM_MACOS)],
                ),
            )
            _, version, arch, os_name = extract_flutter_framework_info(path)
            self.assertEqual((version, arch, os_name), ("3.11.5", "arm64", "macos"))

    def test_platform_falls_back_to_the_version_string(self):
        """Older images carry no LC_BUILD_VERSION."""
        with tempfile.TemporaryDirectory() as tmp:
            path = write(tmp, thin_macho64(CPU_TYPE_ARM64, version_blob("3.10.7", "ios_arm64")))
            _, version, arch, os_name = extract_flutter_framework_info(path)
            self.assertEqual((version, arch, os_name), ("3.10.7", "arm64", "ios"))

    def test_nothing_to_go_on_reports_what_to_do(self):
        """An iOS engine carries no engine ids, so a missing version string
        cannot be recovered. Say so, and name the way out."""
        with tempfile.TemporaryDirectory() as tmp:
            path = write(
                tmp, thin_macho64(CPU_TYPE_ARM64, b"", [build_version_command(PLATFORM_IOS)])
            )
            engine_ids, version, _, _ = extract_flutter_framework_info(path)
            self.assertIsNone(version)
            self.assertEqual(engine_ids, [])


REAL_IOS = os.environ.get("BLUTTER_TEST_IOS_ENGINE")
REAL_MACOS = os.environ.get("BLUTTER_TEST_MACOS_ENGINE")


@unittest.skipUnless(
    extract_flutter_framework_info is not None
    and REAL_IOS
    and pathlib.Path(REAL_IOS).is_file(),
    "set BLUTTER_TEST_IOS_ENGINE to a real Flutter.framework/Flutter",
)
class RealIosEngineTests(unittest.TestCase):
    def test_detection_is_unchanged(self):
        _, version, arch, os_name = extract_flutter_framework_info(REAL_IOS)
        self.assertEqual((arch, os_name), ("arm64", "ios"))
        self.assertIsNotNone(version)

    def test_ios_engines_carry_no_engine_ids(self):
        """Recorded because it is the reason the lookup cannot rescue iOS."""
        engine_ids, _, _, _ = extract_flutter_framework_info(REAL_IOS)
        self.assertEqual(engine_ids, [])


@unittest.skipUnless(
    extract_flutter_framework_info is not None
    and REAL_MACOS
    and pathlib.Path(REAL_MACOS).is_file(),
    "set BLUTTER_TEST_MACOS_ENGINE to a real FlutterMacOS",
)
class RealMacosEngineTests(unittest.TestCase):
    def test_detection_is_unchanged(self):
        _, version, arch, os_name = extract_flutter_framework_info(REAL_MACOS)
        self.assertEqual((arch, os_name), ("arm64", "macos"))
        self.assertIsNotNone(version)

    def test_macos_engines_do_carry_engine_ids(self):
        """Which is what makes the fallback worth having at all."""
        engine_ids, _, _, _ = extract_flutter_framework_info(REAL_MACOS)
        self.assertEqual(len(engine_ids), 2)
        for engine_id in engine_ids:
            self.assertRegex(engine_id, r"^[0-9a-f]{40}$")


if __name__ == "__main__":
    unittest.main()
