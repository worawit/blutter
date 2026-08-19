"""Locating the app and engine binaries in each platform's layout.

Issue #10: a macOS Flutter app ships `FlutterMacOS.framework`, not
`Flutter.framework`, and uses a versioned bundle — `Versions/A/<name>` with a
symlink at the top — where iOS uses a flat one. `find_lib_files()` knew only
the Android and iOS shapes, so a macOS app failed at the first step with
"Cannot find libflutter file".

Layouts are built from empty files: this is about pathfinding, so the contents
are irrelevant and no sample binary is needed.

Imports blutter.py, which pulls in requests and pyelftools; run with the
interpreter Blutter itself uses.
"""

import os
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

# A runner asks for strict mode so a failed dependency install fails the run
# instead of quietly skipping. Sample-driven skips stay skips.
STRICT = os.environ.get("BLUTTER_TESTS_STRICT") == "1"

try:
    from blutter import find_lib_files

    IMPORT_ERROR = None
except ImportError as exc:  # pragma: no cover - depends on the environment
    if STRICT:
        raise
    find_lib_files = None
    IMPORT_ERROR = str(exc)


def make(root: pathlib.Path, *relative_paths: str) -> None:
    """Create empty files, and the directories leading to them."""
    for rel in relative_paths:
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"")


@unittest.skipIf(find_lib_files is None, f"blutter not importable: {IMPORT_ERROR}")
class FindLibFilesTests(unittest.TestCase):
    def assertFound(self, root: pathlib.Path, app_rel: str, flutter_rel: str):
        app, flutter = find_lib_files(str(root))
        self.assertEqual(pathlib.Path(app), (root / app_rel).resolve())
        self.assertEqual(pathlib.Path(flutter), (root / flutter_rel).resolve())

    def test_android_extracted_libs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            make(root, "libapp.so", "libflutter.so")
            self.assertFound(root, "libapp.so", "libflutter.so")

    def test_ios_flat_frameworks(self):
        """What an iOS .app/Frameworks directory looks like."""
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            make(root, "App.framework/App", "Flutter.framework/Flutter")
            self.assertFound(root, "App.framework/App", "Flutter.framework/Flutter")

    def test_macos_versioned_frameworks(self):
        """What a macOS .app/Contents/Frameworks directory looks like.

        Different engine framework name, and a versioned bundle layout.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            make(
                root,
                "App.framework/Versions/A/App",
                "FlutterMacOS.framework/Versions/A/FlutterMacOS",
            )
            self.assertFound(
                root,
                "App.framework/Versions/A/App",
                "FlutterMacOS.framework/Versions/A/FlutterMacOS",
            )

    def test_macos_bundle_with_the_usual_top_level_symlinks(self):
        """A real bundle also has App.framework/App -> Versions/A/App.

        Either resolves to the same file, so either is acceptable; what must not
        happen is failing to find it.
        """
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            make(
                root,
                "App.framework/Versions/A/App",
                "FlutterMacOS.framework/Versions/A/FlutterMacOS",
            )
            os.symlink("Versions/A/App", root / "App.framework" / "App")
            os.symlink(
                "Versions/A/FlutterMacOS",
                root / "FlutterMacOS.framework" / "FlutterMacOS",
            )
            app, flutter = find_lib_files(str(root))
            self.assertEqual(
                pathlib.Path(app).resolve(),
                (root / "App.framework/Versions/A/App").resolve(),
            )
            self.assertEqual(
                pathlib.Path(flutter).resolve(),
                (root / "FlutterMacOS.framework/Versions/A/FlutterMacOS").resolve(),
            )

    def test_missing_engine_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            make(root, "App.framework/Versions/A/App")
            with self.assertRaises(SystemExit) as caught:
                find_lib_files(str(root))
            self.assertIn("libflutter", str(caught.exception))

    def test_missing_app_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            make(root, "FlutterMacOS.framework/Versions/A/FlutterMacOS")
            with self.assertRaises(SystemExit) as caught:
                find_lib_files(str(root))
            self.assertIn("libapp", str(caught.exception))

    def test_a_directory_named_like_the_binary_is_not_mistaken_for_it(self):
        """`App.framework/App` is a directory in some layouts; skip past it."""
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "App").mkdir()
            make(root, "App.framework/Versions/A/App", "Flutter.framework/Flutter")
            self.assertFound(root, "App.framework/Versions/A/App", "Flutter.framework/Flutter")


if __name__ == "__main__":
    unittest.main()
