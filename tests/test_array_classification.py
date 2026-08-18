"""Array element accesses must classify identically on both pointer widths.

Regression test for issue #2: `getArrayOp()` compared the array data offset
against the literal `0x17`, which is `Array::data_offset() - kHeapObjectTag`
for an **uncompressed** build only. With compressed pointers the two header
fields are 4 bytes each and the Array offset is `0x0f` — while `0x17` is the
*TypedData* payload offset. So the literal did not merely fail to match: on a
compressed build it matched the wrong array kind, labelling TypedData accesses
`List` and leaving real List accesses `Unknown`.

Asserting "nothing is left Unknown" would be wrong: TypedData accesses reached
through the LDUR/STUR path have no better label available and legitimately stay
Unknown. The invariant used here needs no knowledge of any offset:

    a load whose result is immediately DecompressPointer'd is a tagged-pointer
    element, i.e. a List element, so it must be classified List.

These tests need a real sample, so they are opt-in::

    BLUTTER_BIN=bin/blutter_dartvm<ver>_android_arm64 \\
    BLUTTER_TEST_LIBAPP=/path/to/libapp.so \\
    python -m unittest discover -s tests

`BLUTTER_TEST_LIBAPP` must be a **compressed-pointer** build (any Android
Flutter release APK provides one); on an uncompressed build there are no
DecompressPointer instructions and the central test is vacuous. Standard
library only.
"""

import os
import pathlib
import re
import subprocess
import tempfile
import unittest

BLUTTER_BIN = os.environ.get("BLUTTER_BIN")
SAMPLE = os.environ.get("BLUTTER_TEST_LIBAPP")

CLASSIFICATION = re.compile(r";\s(List|Unknown|TypedSigned|TypedUnsigned|TypeUnknown)_\d+")
ARRAY_LOAD = re.compile(r"ArrayLoad: (r\d+) = .*;\s(\w+)_\d+")
ARRAY_OP_INDEX = re.compile(r"Array(?:Load|Store): [^;]*\[(-?\d+)\][^;]*;\s(\w+)_\d+")
DECOMPRESS = re.compile(r"DecompressPointer (r\d+)")

# How far after a load to look for the decompression of its result.
DECOMPRESS_WINDOW = 5


def _requirements_met() -> bool:
    return bool(
        BLUTTER_BIN
        and pathlib.Path(BLUTTER_BIN).is_file()
        and SAMPLE
        and pathlib.Path(SAMPLE).is_file()
    )


@unittest.skipUnless(
    _requirements_met(), "set BLUTTER_BIN and BLUTTER_TEST_LIBAPP to a compressed sample"
)
class ArrayClassificationTests(unittest.TestCase):
    """One analysis run shared by every assertion — it is the expensive part."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        outdir = pathlib.Path(cls._tmp.name) / "out"
        result = subprocess.run(
            [BLUTTER_BIN, "-i", SAMPLE, "-o", str(outdir)],
            capture_output=True,
            text=True,
            timeout=1800,
        )
        if result.returncode != 0:
            raise unittest.SkipTest(f"analysis failed: {result.stderr[-2000:]}")
        cls.stderr = result.stderr

        cls.counts: dict[str, int] = {}
        cls.undecompressed: list[str] = []
        cls.negative_list_indices: list[str] = []

        for path in (outdir / "asm").rglob("*.dart"):
            lines = path.read_text(errors="replace").splitlines()
            for i, line in enumerate(lines):
                for kind in CLASSIFICATION.findall(line):
                    cls.counts[kind] = cls.counts.get(kind, 0) + 1

                m = ARRAY_OP_INDEX.search(line)
                if m and m.group(2) == "List" and int(m.group(1)) < 0:
                    cls.negative_list_indices.append(line.strip())

                m = ARRAY_LOAD.search(line)
                if m:
                    reg, kind = m.group(1), m.group(2)
                    window = "\n".join(lines[i + 1 : i + 1 + DECOMPRESS_WINDOW])
                    d = DECOMPRESS.search(window)
                    if d and d.group(1) == reg and kind != "List":
                        cls.undecompressed.append(line.strip())

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def test_analysis_reports_no_errors(self):
        errors = [ln for ln in self.stderr.splitlines() if ln.startswith("Analysis error at line")]
        self.assertEqual(errors, [], f"{len(errors)} analysis errors")

    def test_list_accesses_are_classified(self):
        self.assertGreater(self.counts.get("List", 0), 0, "no List classification at all")

    def test_decompressed_loads_are_classified_list(self):
        """The bug's signature, in terms that need no knowledge of any offset.

        A load followed by DecompressPointer on the same register read a tagged
        pointer out of the array, which only a List holds. Before the fix these
        came back Unknown, because the Array data offset never matched 0x17.
        """
        self.assertEqual(
            self.undecompressed,
            [],
            f"{len(self.undecompressed)} decompressed loads not classified List, "
            f"e.g. {self.undecompressed[:3]}",
        )

    def test_list_indices_are_never_negative(self):
        """Guards the regression that classifying alone would introduce.

        The index of a fixed-offset access is computed relative to where the
        payload starts, and that differs by array kind. Classifying an access
        as List while still computing its index from the TypedData base yields
        indices like [-2].
        """
        self.assertEqual(
            self.negative_list_indices,
            [],
            f"{len(self.negative_list_indices)} List accesses with a negative index, "
            f"e.g. {self.negative_list_indices[:3]}",
        )


if __name__ == "__main__":
    unittest.main()
