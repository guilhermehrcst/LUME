#!/usr/bin/env python3
"""Atomically publish a directory without replacing any destination entry.

Linux/glibc renameat2(RENAME_NOREPLACE) is required. Never downgrade to
os.rename, shutil.move, or a check-then-rename fallback. If the filesystem
does not support this guarantee (or source and target are on different
filesystems), refuse and leave the staging directory intact.
"""
import ctypes
import os
import sys

AT_FDCWD = -100
RENAME_NOREPLACE = 1


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: rename_noreplace.py <staging-directory> <final-directory>", file=sys.stderr)
        return 2
    src, dest = sys.argv[1:]
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        renameat2 = libc.renameat2
    except (OSError, AttributeError) as exc:
        print(f"REFUSED: renameat2(RENAME_NOREPLACE) unavailable: {exc}", file=sys.stderr)
        return 1
    renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    renameat2.restype = ctypes.c_int
    if renameat2(AT_FDCWD, os.fsencode(src), AT_FDCWD, os.fsencode(dest), RENAME_NOREPLACE) != 0:
        err = ctypes.get_errno()
        print(
            f"REFUSED: atomic no-replace publication failed for {dest}: "
            f"{os.strerror(err)} (errno={err}); staging remains at {src}",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
