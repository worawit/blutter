#!/usr/bin/env python3
"""Patch blutter-generated addNames.py for IDA 9.x compatibility.

IDA 9.x removed the `ida_struct` module. This script replaces the bare
`import ida_struct` with a try/except shim that falls back to
`ida_typeinf`, and inserts a flush() call before the function return
so that all 'set_member_cmt' changes are persisted in one shot.

Usage:
    python patch_ida9.py <blutter-out-dir>
    python patch_ida9.py E:/work/blutter-out

The script modifies <blutter-out-dir>/ida_script/addNames.py in place
and creates a .bak backup before writing.
"""
import sys, shutil
from pathlib import Path

SHIM = r"""try:
    import ida_struct
    _IDA9 = False
except ImportError:
    import ida_typeinf as _ti
    _IDA9 = True
    class _StructShim:
        _pool_tif = None
        @staticmethod
        def get_struc(sid):
            t = _ti.tinfo_t()
            if t.get_type_by_tid(sid):
                _StructShim._pool_tif = t
                return t
            return None
        @staticmethod
        def get_member(s, o):
            return (s, o)
        @staticmethod
        def set_member_cmt(m, c, r):
            t, o = m
            if t is None: return
            u = _ti.udm_t()
            u.offset = o * 8
            i = t.find_udm(u, _ti.STRMEM_OFFSET)
            if i >= 0:
                t.set_udm_cmt(i, c)
        @staticmethod
        def flush():
            t = _StructShim._pool_tif
            if t is not None:
                t.set_named_type(_ti.get_idati(), 'DartObjectPool', _ti.NTF_REPLACE)
    ida_struct = _StructShim()"""

def patch(script: Path):
    text = script.read_text(encoding="utf-8")

    if "_IDA9" in text:
        print(f"[skip] {script} already patched")
        return False

    if "import ida_struct" not in text:
        print(f"[skip] {script} has no 'import ida_struct'")
        return False

    # 1) Replace the bare import with the try/except shim
    text = text.replace("import ida_struct\n", SHIM + "\n", 1)

    # 2) Insert flush() before 'return sid1, sid2' inside create_Dart_structs
    text = text.replace(
        "\treturn sid1, sid2\n",
        "\tif _IDA9: ida_struct.flush()\n\treturn sid1, sid2\n",
        1,
    )

    # 3) Fix invalid escape sequences in set_member_cmt comment strings
    import re
    def _fix_escapes(m):
        s = m.group(1)
        s = s.replace("\\", "\\\\")
        return f"'''{s}'''"
    text = re.sub(r"'''((?:(?!''').)*\\(?:(?!''').)*)'''", _fix_escapes, text)

    backup = script.with_suffix(".py.bak")
    shutil.copy2(script, backup)
    script.write_text(text, encoding="utf-8")
    print(f"[done] patched {script}  (backup: {backup.name})")
    return True

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <blutter-out-dir>")
        sys.exit(1)
    outdir = Path(sys.argv[1])
    script = outdir / "ida_script" / "addNames.py"
    if not script.exists():
        print(f"[error] {script} not found")
        sys.exit(1)
    patch(script)

if __name__ == "__main__":
    main()
