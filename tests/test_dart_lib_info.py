"""Defaults chosen for a Dart VM build when the snapshot flags are unavailable.

`DartLibInfo` derives pointer compression from the target when the caller does
not supply it — the `--dart-version` path, where there is no snapshot to read
flags from.

Android uses compressed pointers; iOS does not. macOS does not either, which
the default originally got wrong by testing only for iOS. Checked against a
real app: AppFlowy 0.13.1 (Dart 3.11.5, macos arm64) ships

    ... arm64 macos no-compressed-pointers

Standard library only, but imports dartvm_fetch_build; run with the interpreter
Blutter itself uses.
"""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

try:
    from dartvm_fetch_build import DartLibInfo

    IMPORT_ERROR = None
except ImportError as exc:  # pragma: no cover - depends on the environment
    DartLibInfo = None
    IMPORT_ERROR = str(exc)


@unittest.skipIf(DartLibInfo is None, f"dartvm_fetch_build not importable: {IMPORT_ERROR}")
class CompressedPointerDefaultTests(unittest.TestCase):
    def test_android_defaults_to_compressed(self):
        self.assertTrue(DartLibInfo("3.11.5", "android", "arm64").has_compressed_ptrs)

    def test_ios_defaults_to_uncompressed(self):
        self.assertFalse(DartLibInfo("3.10.7", "ios", "arm64").has_compressed_ptrs)

    def test_macos_defaults_to_uncompressed(self):
        self.assertFalse(DartLibInfo("3.11.5", "macos", "arm64").has_compressed_ptrs)

    def test_an_explicit_value_always_wins(self):
        """The normal path reads the snapshot flags and passes them in."""
        self.assertTrue(DartLibInfo("3.11.5", "macos", "arm64", True).has_compressed_ptrs)
        self.assertFalse(DartLibInfo("3.11.5", "android", "arm64", False).has_compressed_ptrs)

    def test_lib_name_carries_the_target(self):
        info = DartLibInfo("3.11.5", "macos", "arm64")
        self.assertEqual(info.lib_name, "dartvm3.11.5_macos_arm64")


if __name__ == "__main__":
    unittest.main()
