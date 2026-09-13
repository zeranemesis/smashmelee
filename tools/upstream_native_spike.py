#!/usr/bin/env python3
"""Measure how much of doldecomp/melee compiles for a native host target.

The port's long-term question is whether Melee's decompiled C can be compiled
directly against a host Dolphin SDK instead of being reimplemented.  This
script answers it with numbers rather than opinion: it runs a syntax-only
compile over every ``.c`` file in an upstream checkout and groups whatever
fails by cause.

It never modifies the upstream checkout and never copies upstream code into
this repository.  The two host shims it needs are generated into a temporary
directory that shadows ``src/Runtime/platform.h`` on the include path:

  * the upstream header unconditionally declares ``ssize_t`` as ``int``, which
    collides with the host libc declaration;
  * ``extern/dolphin/include/libc/math.h`` defines ``fabsf`` as a macro, which
    rewrites the host ``math.h`` declaration.

Usage:
    tools/upstream_native_spike.py /path/to/doldecomp/melee [--cc clang]
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile

SHIM_PLATFORM = """\
#pragma once

/* Host shim for the native-compile spike; see tools/upstream_native_spike.py.
   The upstream header declares ssize_t as int, so rename it for the duration
   of that header and let the host libc provide the real one. */
#include <stdint.h>
#include <sys/types.h>

#define ssize_t melee_upstream_ssize_t
#include "{real}"
#undef ssize_t
"""

SHIM_FORCE = """\
/* Parsed before every translation unit: take the host declarations first, then
   drop the Metrowerks fabsf macro that would rewrite them. */
#include <math.h>
#include <stdint.h>
#include <sys/types.h>
#undef fabsf
"""

# Ordered: the first pattern that matches an error line names the cause.
CAUSES = [
    ("GameCube struct layout asserted (32-bit pointers)",
     re.compile(r"static assertion failed.*(offsetof|sizeof)")),
    ("declaration mismatch stricter than MWCC accepts",
     re.compile(r"conflicting types for")),
    ("Metrowerks libc internals", re.compile(r"_IO_FILE|__io_proc|__idle_proc|"
                                             r"__file_handle")),
    ("asset blob not in the repository", re.compile(r"\.inc' file not found")),
    ("incompatible function pointer type",
     re.compile(r"incompatible function pointer types")),
]


def classify(output: str) -> str:
    for line in output.splitlines():
        if "error:" not in line:
            continue
        for name, pattern in CAUSES:
            if pattern.search(line):
                return name
        return "other: " + line.split("error:", 1)[1].strip()[:70]
    return "other"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("upstream", type=pathlib.Path,
                        help="path to a doldecomp/melee checkout")
    parser.add_argument("--cc", default="clang", help="host C compiler")
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()

    upstream = args.upstream.resolve()
    source = upstream / "src"
    if not (source / "Runtime" / "platform.h").exists():
        print(f"{upstream} does not look like a doldecomp/melee checkout",
              file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as scratch:
        shim_root = pathlib.Path(scratch)
        (shim_root / "Runtime").mkdir()
        (shim_root / "Runtime" / "platform.h").write_text(
            SHIM_PLATFORM.format(real=source / "Runtime" / "platform.h"))
        force = shim_root / "force.h"
        force.write_text(SHIM_FORCE)

        command = [
            args.cc, "-fsyntax-only", "-std=gnu11", "-w",
            "-Wno-incompatible-function-pointer-types",
            f"-I{shim_root}", f"-I{source}",
            f"-I{upstream / 'extern' / 'dolphin' / 'include'}",
            f"-I{source / 'sysdolphin' / 'baselib'}",
            "-DNDEBUG=1", "-DVERSION_NTSC102", "-include", str(force),
        ]

        files = sorted(p for p in source.rglob("*.c")
                       if p.is_relative_to(source / "melee")
                       or p.is_relative_to(source / "sysdolphin"))

        def check(path: pathlib.Path) -> tuple[pathlib.Path, str | None]:
            result = subprocess.run(command + [str(path)],
                                    capture_output=True, text=True)
            if result.returncode == 0:
                return path, None
            return path, classify(result.stderr)

        failures: list[tuple[pathlib.Path, str]] = []
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            for path, cause in pool.map(check, files):
                if cause is not None:
                    failures.append((path, cause))

    passed = len(files) - len(failures)
    print(f"{passed}/{len(files)} upstream translation units pass a native "
          f"syntax check ({passed * 100.0 / len(files):.1f}%)\n")

    by_cause = collections.Counter(cause for _, cause in failures)
    print("Failures by cause:")
    for cause, count in by_cause.most_common():
        print(f"  {count:4d}  {cause}")

    by_module = collections.Counter(
        str(path.relative_to(source).parent) for path, _ in failures)
    print("\nFailures by module:")
    for module, count in by_module.most_common(12):
        print(f"  {count:4d}  {module}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
