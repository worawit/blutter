"""Blutter must create the output directory tree, not just its last component.

Regression test for the failure where an output path whose parent does not yet
exist aborts the run with

    Failed to create output directory: No such file or directory

after the expensive Dart VM build has already happened.

The directory is created before the snapshot is loaded, so these tests pass a
deliberately invalid input file: the run is expected to fail *later*, and what
is asserted is that it does not fail at directory creation. That keeps the test
fast — no snapshot parsing, no analysis — and independent of any test binary.

Standard library only. Run with::

    python -m unittest discover -s tests

Set BLUTTER_BIN to test a specific executable; otherwise the newest one in
bin/ is used.
"""

import os
import pathlib
import subprocess
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN_DIR = REPO_ROOT / "bin"

DIRECTORY_FAILURE = "Failed to create output directory"


def find_blutter() -> pathlib.Path | None:
    """The executable under test: $BLUTTER_BIN, else the newest built binary."""
    override = os.environ.get("BLUTTER_BIN")
    if override:
        path = pathlib.Path(override)
        return path if path.is_file() else None

    if not BIN_DIR.is_dir():
        return None
    candidates = [p for p in BIN_DIR.iterdir() if p.is_file() and os.access(p, os.X_OK)]
    if not candidates:
        return None
    return max(candidates, key=lambda p: p.stat().st_mtime)


BLUTTER = find_blutter()


@unittest.skipIf(BLUTTER is None, "no built Blutter executable in bin/ (set BLUTTER_BIN)")
class OutputDirectoryTests(unittest.TestCase):
    def run_blutter(self, outdir: pathlib.Path) -> subprocess.CompletedProcess:
        """Run with an input that cannot load, so only the outdir logic runs."""
        assert BLUTTER is not None
        with tempfile.NamedTemporaryFile(suffix=".so") as bogus_input:
            bogus_input.write(b"not an ELF or Mach-O file")
            bogus_input.flush()
            return subprocess.run(
                [str(BLUTTER), "-i", bogus_input.name, "-o", str(outdir)],
                capture_output=True,
                text=True,
                timeout=120,
            )

    def test_creates_a_missing_parent(self):
        """The reported bug: outdir whose parent does not exist."""
        with tempfile.TemporaryDirectory() as tmp:
            outdir = pathlib.Path(tmp) / "missing_parent" / "out"
            result = self.run_blutter(outdir)
            self.assertNotIn(DIRECTORY_FAILURE, result.stderr)
            self.assertTrue(outdir.is_dir(), f"{outdir} was not created")

    def test_creates_a_deeply_nested_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            outdir = pathlib.Path(tmp) / "a" / "b" / "c" / "d" / "out"
            result = self.run_blutter(outdir)
            self.assertNotIn(DIRECTORY_FAILURE, result.stderr)
            self.assertTrue(outdir.is_dir(), f"{outdir} was not created")

    def test_existing_directory_is_not_an_error(self):
        """Re-running into a previous run's output must still work."""
        with tempfile.TemporaryDirectory() as tmp:
            outdir = pathlib.Path(tmp) / "already" / "here"
            outdir.mkdir(parents=True)
            result = self.run_blutter(outdir)
            self.assertNotIn(DIRECTORY_FAILURE, result.stderr)
            self.assertTrue(outdir.is_dir())

    def test_single_component_under_an_existing_parent_still_works(self):
        """The case that already worked, so the fix does not regress it."""
        with tempfile.TemporaryDirectory() as tmp:
            outdir = pathlib.Path(tmp) / "out"
            result = self.run_blutter(outdir)
            self.assertNotIn(DIRECTORY_FAILURE, result.stderr)
            self.assertTrue(outdir.is_dir(), f"{outdir} was not created")

    def test_output_path_that_is_a_file_is_still_reported(self):
        """A genuine failure must still be reported, and this one works as root."""
        with tempfile.TemporaryDirectory() as tmp:
            blocker = pathlib.Path(tmp) / "out"
            blocker.write_text("I am a file, not a directory")
            result = self.run_blutter(blocker)
            self.assertIn(DIRECTORY_FAILURE, result.stderr)
            self.assertEqual(result.returncode, 1)

    def test_parent_component_that_is_a_file_is_still_reported(self):
        """Same, one level up: a non-directory in the middle of the path."""
        with tempfile.TemporaryDirectory() as tmp:
            blocker = pathlib.Path(tmp) / "blocker"
            blocker.write_text("I am a file, not a directory")
            result = self.run_blutter(blocker / "out")
            self.assertIn(DIRECTORY_FAILURE, result.stderr)
            self.assertEqual(result.returncode, 1)

    def test_unwritable_parent_is_still_reported(self):
        """Fixing the missing-parent case must not swallow genuine failures."""
        if os.geteuid() == 0:
            self.skipTest("running as root: permission checks do not apply")
        with tempfile.TemporaryDirectory() as tmp:
            locked = pathlib.Path(tmp) / "locked"
            locked.mkdir(mode=0o500)
            result = self.run_blutter(locked / "out")
            self.assertIn(DIRECTORY_FAILURE, result.stderr)
            self.assertEqual(result.returncode, 1)


if __name__ == "__main__":
    unittest.main()
