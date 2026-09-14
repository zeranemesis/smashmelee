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

``--fobj-reference`` is narrower and sharper: it builds upstream's HSD core
against Aurora's Dolphin headers, runs its animation interpreter over a table
of bytecode streams, and prints what it produces.  Those values are what
tests/hsd/test_fobj.cpp asserts, so the port is pinned to the original
implementation rather than to a reading of the format.

``--pointer-casts`` reports where upstream truncates a pointer through a 32-bit
integer.  Those casts, not the structure-layout assertions, are the real
inventory of what a 64-bit host has to change.

Usage:
    tools/upstream_native_spike.py /path/to/doldecomp/melee [--cc clang]
    tools/upstream_native_spike.py /path/to/doldecomp/melee --link-surface
    tools/upstream_native_spike.py /path/to/doldecomp/melee --fobj-reference
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import os
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
/* Parsed before every translation unit when Aurora's headers are NOT in use.
   The --aurora-headers path force-includes the repository's real prelude
   instead, so the tool measures the configuration the build actually
   compiles. */
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


# The HSD units the port depends on, in the order phase 2 of docs/PLAN.md
# brings them up.  The totals across sysdolphin are dominated by the particle
# system and the debug console, which the port does not reach, so this set is
# reported on its own.
CORE_UNITS = (
    "objalloc", "class", "object", "list", "id", "mtx", "fobj", "aobj",
    "jobj", "dobj", "mobj", "tobj", "pobj", "cobj", "lobj", "robj",
    "archive", "tev", "texp", "shadow", "spline", "quatlib", "random",
    "util", "state", "memory", "hash",
)

POINTER_CAST_WARNINGS = (
    "-Wpointer-to-int-cast", "-Wint-to-pointer-cast",
    "-Wvoid-pointer-to-int-cast", "-Wint-to-void-pointer-cast",
)


def pointer_casts(files: list[pathlib.Path], source: pathlib.Path,
                  command: list[str]) -> int:
    """Counts the 32-bit pointer casts in each translation unit."""
    per_unit: collections.Counter = collections.Counter()
    sites: dict[str, list[str]] = collections.defaultdict(list)

    # The shared command silences warnings wholesale; these are the ones the
    # inventory is made of, so drop that and re-enable only them.
    loud = [argument for argument in command if argument != "-w"]

    def scan(path: pathlib.Path) -> tuple[str, int, list[str]]:
        result = subprocess.run(
            loud + ["-c", "-O0", "-o", os.devnull, "-Wno-everything",
                    *POINTER_CAST_WARNINGS, str(path)],
            capture_output=True, text=True)
        unit = path.stem
        lines = [line.strip() for line in result.stderr.splitlines()
                 if "warning:" in line]
        return unit, len(lines), lines

    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        for unit, count, lines in pool.map(scan, files):
            if count:
                per_unit[unit] = count
                sites[unit] = lines

    total = sum(per_unit.values())
    core = {unit: count for unit, count in per_unit.items()
            if unit in CORE_UNITS}
    print(f"{total} pointer-truncating casts across {len(per_unit)} of "
          f"{len(files)} translation units\n")
    print(f"In the {len(CORE_UNITS)} units the port depends on: "
          f"{sum(core.values())} casts in {len(core)} of them")
    for unit, count in sorted(core.items(), key=lambda kv: -kv[1]):
        print(f"  {count:4d}  {unit}")
    print("\nElsewhere (subsystems the port does not reach yet):")
    for unit, count in sorted(per_unit.items(), key=lambda kv: -kv[1]):
        if unit not in CORE_UNITS:
            print(f"  {count:4d}  {unit}")
    return 0


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


# The whole host surface upstream's HSD core needs in order to run.
HOST_SHIMS = r"""
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

volatile int __OSCurrHeap = 0;

void* OSAllocFromHeap(int heap, unsigned long size)
{
    (void) heap;
    void* memory = NULL;
    if (posix_memalign(&memory, 32, size ? size : 32) != 0) return NULL;
    return memory;
}

void OSFreeToHeap(int heap, void* pointer) { (void) heap; free(pointer); }
long OSCheckHeap(int heap) { (void) heap; return 0; }
int HSD_GetHeap(void) { return 0; }

void OSReport(char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
}

void __assert(char* file, unsigned int line, char* message)
{
    fprintf(stderr, "upstream assert: %s:%u: %s\n", file, line, message);
    abort();
}
"""

# Mirrors the table in tests/hsd/test_fobj.cpp.  The first byte of a pack
# carries the opcode in its low nibble and the key count minus one in bits 4-6.
FOBJ_DRIVER = r"""
#include <stdio.h>
#include <string.h>
#include <sysdolphin/baselib/objalloc.h>
#include <sysdolphin/baselib/fobj.h>

typedef struct { const char* name; const unsigned char* bytes;
                 unsigned length; unsigned char frac; } Case;

static const unsigned char c0[] = { 0x10 | 1, 7, 1, 9, 1 };
static const unsigned char c1[] = { 0x20 | 1, 1, 1, 2, 3, 3, 1 };
static const unsigned char c2[] = { 0x10 | 2, 0, 2, 4, 1 };
static const unsigned char c3[] = { 0x20 | 2, 0, 4, 10, 2, 20, 1 };
static const unsigned char c4[] = { 0x10 | 3, 0, 2, 8, 1 };
static const unsigned char c5[] = { 0x10 | 4, 0, 2, 2, 8, 1, 1 };
static const unsigned char c6[] = { 0x00 | 5, 3, 1, 0x10 | 1, 4, 2, 9, 1 };
static const unsigned char c7[] = { 0x10 | 6, 5, 2, 9, 1 };
static const unsigned char c8[] = { 0x10 | 1, 7, 0x84, 0x01, 9, 1 };
static const unsigned char c9[] = { 0x10 | 1, 2, 1, 4, 1, 0x10 | 2, 8, 2, 16, 1 };

static const Case cases[] = {
    { "constant, two keys", c0, sizeof(c0), 0x80 },
    { "constant, three keys", c1, sizeof(c1), 0x80 },
    { "linear, two keys", c2, sizeof(c2), 0x80 },
    { "linear, three keys", c3, sizeof(c3), 0x80 },
    { "spline without slopes", c4, sizeof(c4), 0x80 },
    { "spline with slopes", c5, sizeof(c5), 0x80 },
    { "slope then constant", c6, sizeof(c6), 0x80 },
    { "key", c7, sizeof(c7), 0x80 },
    { "multi-byte wait", c8, sizeof(c8), 0x80 },
    { "constant pack then linear pack", c9, sizeof(c9), 0x80 },
    { "two fraction bits", c0, sizeof(c0), 0x82 },
    { "signed eight-bit values", c0, sizeof(c0), 0x60 },
};

static void show(void* o, int type, HSD_ObjData* v)
{ (void) o; (void) type; printf(" %.6g", (double) v->fv); }

int main(void)
{
    unsigned i, t;
    HSD_FObjInitAllocData();
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        static unsigned char buffer[64];
        HSD_FObj* channel;
        memcpy(buffer, cases[i].bytes, cases[i].length);
        channel = HSD_FObjAlloc();
        memset(channel, 0, sizeof(*channel));
        channel->ad_head = buffer;
        channel->length = cases[i].length;
        channel->frac_value = cases[i].frac;
        channel->frac_slope = cases[i].frac;
        channel->obj_type = 5;
        HSD_FObjReqAnimAll(channel, 0.0f);
        printf("%-32s:", cases[i].name);
        for (t = 0; t < 8; ++t) {
            HSD_FObjInterpretAnim(channel, NULL, show, t == 0 ? 0.0f : 1.0f);
        }
        printf("   [state %u]\n", (unsigned) HSD_FObjGetState(channel));
        HSD_FObjRemove(channel);
    }
    return 0;
}
"""

# The units upstream's animation interpreter needs in order to link.
FOBJ_UNITS = ("objalloc", "list", "id", "fobj", "class", "object", "memory",
              "hash", "spline")


def fobj_reference(upstream: pathlib.Path, compiler: str, scratch: pathlib.Path,
                   command: list[str]) -> int:
    """Builds upstream's HSD core and prints what its interpreter produces."""
    source = upstream / "src"
    shims = scratch / "host_shims.c"
    shims.write_text(HOST_SHIMS)
    driver = scratch / "fobj_driver.c"
    driver.write_text(FOBJ_DRIVER)

    objects = []
    for unit in FOBJ_UNITS:
        target = scratch / f"{unit}.o"
        result = subprocess.run(
            command + ["-c", "-O0", "-o", str(target),
                       str(source / "sysdolphin" / "baselib" / f"{unit}.c")],
            capture_output=True, text=True)
        if result.returncode != 0:
            print(f"could not compile upstream {unit}.c:\n{result.stderr}",
                  file=sys.stderr)
            return 1
        objects.append(str(target))

    binary = scratch / "fobj_reference"
    result = subprocess.run(
        command + [str(driver), str(shims), *objects, "-lm", "-o", str(binary)],
        capture_output=True, text=True)
    if result.returncode != 0:
        print(f"could not link the reference:\n{result.stderr}",
              file=sys.stderr)
        return 1

    print("upstream sysdolphin/baselib/fobj.c, running natively:\n")
    return subprocess.run([str(binary)]).returncode


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
    parser.add_argument("--aurora-headers", action="store_true",
                        help="compile against Aurora's Dolphin headers, which "
                             "is the configuration a host port would use")
    parser.add_argument("--only", default=None,
                        help="restrict to sources under this path, relative "
                             "to the checkout's src/ (e.g. sysdolphin/baselib)")
    parser.add_argument("--pointer-casts", action="store_true",
                        help="report where upstream truncates a pointer "
                             "through a 32-bit integer")
    parser.add_argument("--fobj-reference", action="store_true",
                        help="build upstream's HSD core and print what its "
                             "animation interpreter produces")
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
        repository = pathlib.Path(__file__).resolve().parent.parent
        aurora = repository / "extern" / "aurora" / "include"
        use_aurora = (args.aurora_headers or args.fobj_reference
                      or args.pointer_casts)
        if use_aurora:
            if not (aurora / "dolphin" / "types.h").exists():
                print("run: git submodule update --init --depth 1 "
                      "extern/aurora", file=sys.stderr)
                return 2
            # The repository's real prelude, not a copy of it.
            #
            # This used to be a second transcription of
            # include/melee/port/dolphin_compat.h, and it had drifted: the
            # build had grown _USE_MATH_DEFINES, the M_PI fallbacks, the GX
            # umbrella include, the GXSetArray adapter and a dozen SDK
            # spellings this file never learned.  So the tool was measuring a
            # configuration nobody builds, and reporting failures the real
            # build does not have.  One copy now.
            prelude = (repository / "include" / "melee" / "port" /
                       "dolphin_compat.h")
            if not prelude.is_file():
                print(f"missing prelude: {prelude}", file=sys.stderr)
                return 2
            force.write_text(f'#include "{prelude}"\n')
        else:
            force.write_text(SHIM_FORCE)

        command = [
            args.cc, "-std=gnu11", "-w",
            "-Wno-incompatible-function-pointer-types",
            f"-I{shim_root}",
            *([f"-I{aurora}", f"-I{aurora / 'dolphin'}"] if use_aurora else []),
            f"-I{source}",
            f"-I{upstream / 'extern' / 'dolphin' / 'include'}",
            f"-I{source / 'sysdolphin' / 'baselib'}",
            "-DNDEBUG=1", "-DVERSION_NTSC102",
            *(["-DTARGET_PC"] if use_aurora else []),
            "-include", str(force),
        ]

        if args.fobj_reference:
            return fobj_reference(upstream, args.cc, shim_root, command)

        roots = ([source / args.only] if args.only
                 else [source / "melee", source / "sysdolphin"])
        files = sorted(p for p in source.rglob("*.c")
                       if any(p.is_relative_to(root) for root in roots))
        if not files:
            print(f"no sources under {roots}", file=sys.stderr)
            return 2

        if args.pointer_casts:
            return pointer_casts(files, source, command)

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
    headers = "Aurora's" if (args.aurora_headers or args.fobj_reference) \
        else "upstream's"
    print(f"{passed}/{len(files)} upstream translation units build against "
          f"{headers} Dolphin headers ({passed * 100.0 / len(files):.1f}%)\n")

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
