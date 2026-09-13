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

With ``--link-surface`` it goes further: it compiles real object files,
links their symbol tables together, and reports which Dolphin SDK symbols are
still unresolved and how many of those Aurora already implements.  That number,
not the syntax number, is what bounds a compiled-upstream path.

Usage:
    tools/upstream_native_spike.py /path/to/doldecomp/melee [--cc clang]
    tools/upstream_native_spike.py /path/to/doldecomp/melee --link-surface
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import pathlib
import re
import shutil
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


SDK_PREFIXES = ("GX", "OS", "VI", "DVD", "CARD", "PAD", "SI", "ARQ", "AR",
                "C_", "PS", "MTX", "Mtx", "Vec", "Quat", "DC", "IC", "LC")

FAMILIES = [
    ("GX", ("GX",)), ("OS", ("OS",)), ("VI", ("VI",)), ("DVD", ("DVD",)),
    ("CARD", ("CARD",)), ("PAD", ("PAD",)), ("SI", ("SI",)),
    ("ARAM", ("ARQ", "AR")), ("cache", ("DC", "IC", "LC")),
    ("math", ("C_", "PS", "MTX", "Mtx", "Vec", "Quat")),
]


def family_of(symbol: str) -> str:
    for name, prefixes in FAMILIES:
        if symbol.startswith(prefixes):
            return name
    return "other"


def symbols(command: list[str], objects: list[pathlib.Path],
            selector) -> set[str]:
    """Runs nm over the objects in batches and collects matching names."""
    found: set[str] = set()
    for start in range(0, len(objects), 256):
        batch = [str(path) for path in objects[start:start + 256]]
        result = subprocess.run(command + batch, capture_output=True, text=True)
        for line in result.stdout.splitlines():
            fields = line.split(":", 1)[-1].split()
            name = selector(fields)
            if name is not None:
                found.add(name)
    return found


def aurora_symbols(repository: pathlib.Path) -> set[str]:
    """Function definitions in Aurora's Dolphin SDK implementation."""
    library = repository / "extern" / "aurora" / "lib"
    pattern = re.compile(
        r"^[A-Za-z_][A-Za-z0-9_:<>,*\s]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
    found: set[str] = set()
    for path in list(library.rglob("*.cpp")) + list(library.rglob("*.c")):
        for line in path.read_text(errors="replace").splitlines():
            match = pattern.match(line)
            if match and match.group(1).startswith(SDK_PREFIXES):
                found.add(match.group(1))
    return found


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
    parser.add_argument("--link-surface", action="store_true",
                        help="also compile objects and report the unresolved "
                             "Dolphin SDK symbols")
    parser.add_argument("--nm", default="nm")
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
            args.cc, "-std=gnu11", "-w",
            "-Wno-incompatible-function-pointer-types",
            f"-I{shim_root}", f"-I{source}",
            f"-I{upstream / 'extern' / 'dolphin' / 'include'}",
            f"-I{source / 'sysdolphin' / 'baselib'}",
            "-DNDEBUG=1", "-DVERSION_NTSC102", "-include", str(force),
        ]

        files = sorted(p for p in source.rglob("*.c")
                       if p.is_relative_to(source / "melee")
                       or p.is_relative_to(source / "sysdolphin"))

        objects = shim_root / "obj"
        objects.mkdir()

        def check(path: pathlib.Path) -> tuple[pathlib.Path, str | None]:
            if args.link_surface:
                target = objects / (str(path.relative_to(source)).replace(
                    "/", "_") + ".o")
                extra = ["-c", "-O0", "-o", str(target)]
            else:
                extra = ["-fsyntax-only"]
            result = subprocess.run(command + extra + [str(path)],
                                    capture_output=True, text=True)
            if result.returncode == 0:
                return path, None
            return path, classify(result.stderr)

        failures: list[tuple[pathlib.Path, str]] = []
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            for path, cause in pool.map(check, files):
                if cause is not None:
                    failures.append((path, cause))

        link_report: list[str] = []
        if args.link_surface:
            built = sorted(objects.glob("*.o"))
            nm = shutil.which(args.nm) or args.nm
            defined = symbols([nm, "--defined-only", "-A"], built,
                              lambda f: f[2] if len(f) == 3 else None)
            undefined = symbols([nm, "-u", "-A"], built,
                                lambda f: f[1] if len(f) == 2 and f[0] == "U"
                                else None)
            unresolved = undefined - defined
            sdk = {name for name in unresolved
                   if name.startswith(SDK_PREFIXES)}
            provided = aurora_symbols(pathlib.Path(__file__).resolve().parent
                                      .parent)
            gap = sorted(sdk - provided)

            link_report.append(
                f"\n{len(built)} objects define {len(defined)} symbols and "
                f"reference {len(undefined)} externals.")
            link_report.append(
                f"{len(unresolved)} stay unresolved once they are linked "
                f"together, {len(sdk)} of them Dolphin SDK symbols.")
            link_report.append(
                f"Aurora implements {len(sdk) - len(gap)} of those "
                f"{len(sdk)}; {len(gap)} are missing:")
            by_family = collections.Counter(family_of(name) for name in gap)
            for name, count in by_family.most_common():
                members = [symbol for symbol in gap if family_of(symbol) == name]
                link_report.append(f"  {count:4d}  {name}: " +
                                   " ".join(members[:6]) +
                                   (" ..." if count > 6 else ""))

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
    for line in link_report:
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
