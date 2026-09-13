# Can the upstream decompilation be compiled natively?

## Why this matters

The port currently reimplements `sysdolphin/baselib` in C++ and decodes DAT
archives into host-owned structures. That is sound, but Melee is roughly
490,000 lines of C across 1,034 files; reimplementing it by hand does not
reach a complete game in any reasonable time.

`doldecomp/melee` is decompiled to C essentially in full — there is no `asm/`
tree, two `.s` files remain (both Metrowerks runtime support), and eight files
carry small inline-assembly fragments. So the alternative is to compile that C
directly against a host Dolphin SDK, which is what the M1 exit criteria in
[MELEE_PORT.md](MELEE_PORT.md) already describe.

This document records what was actually measured, not what seems plausible.

## Method

`tools/upstream_native_spike.py` runs a syntax-only compile of every `.c` file
under `src/melee` and `src/sysdolphin` of an upstream checkout, using the host
compiler and host libc, and groups the failures by cause:

```sh
git clone --depth 1 https://github.com/doldecomp/melee.git /tmp/melee-upstream
tools/upstream_native_spike.py /tmp/melee-upstream
```

It does not modify the checkout and copies no upstream code into this
repository. Two host shims are generated into a temporary directory that
shadows `src/Runtime/platform.h` on the include path:

- the upstream header declares `ssize_t` as `int`, which collides with the
  host libc declaration;
- `extern/dolphin/include/libc/math.h` defines `fabsf` as a macro, which
  rewrites the host `math.h` declaration of the same name.

Both are one-line accommodations a host port would carry permanently.

## Result

With clang 18 targeting x86-64:

```
940/984 upstream translation units pass a native syntax check (95.5%)

Failures by cause:
    33  GameCube struct layout asserted (32-bit pointers)
     8  declaration mismatch stricter than MWCC accepts
     2  asset blob not in the repository
     1  Metrowerks libc internals

Failures by module:
    18  melee/gm     9  melee/gr     4  melee/ty     4  sysdolphin/baselib
     3  melee/if     3  melee/vi     2  melee/mn     1  melee/lb
```

## What this does and does not show

It shows that the decompiled game's **types, headers, and declarations**
resolve against a host toolchain almost everywhere. The eight declaration
mismatches are upstream bugs that only a compiler stricter than MWCC reports,
and the two missing blobs are font data the decomp does not redistribute.

It does **not** show that the game builds, links, or runs. A syntax check
resolves no symbol: the full Dolphin SDK surface (OS, VI, DVD, PAD, CARD, GX,
AX, ARQ, matrix) has to exist behind it, and Aurora only covers part of that.

The 33 layout failures are the substantive result, and they are not noise.
The decompilation asserts the console's structure offsets because DAT archives
store 32-bit pointers that HSD relocates **in place**, and game structures are
then overlaid directly on that data. Widening every pointer to 64 bits moves
every field after it, which is precisely why this port decodes archives into
host-owned structures instead.

Two ways out, with their real costs:

- **Compile the game code for a 32-bit target.** Pointer width matches the
  console, the layout assertions hold, and in-place relocation becomes
  possible again. Byte order is still wrong — DAT data is big-endian — so
  every multi-word read still has to be swapped, either at load time or at
  each access. Cost: a 32-bit build of the host runtime too, on every
  platform that must ship.
- **Compile for 64-bit and drop the in-place model.** Archives are decoded
  into host-owned structures, as the port does today, and the layout
  assertions are disabled for the host build. Cost: every HSD structure the
  game touches needs a host-side counterpart and a decoder.

A 32-bit measurement was attempted and is not reported here: this container
has no 32-bit libc headers, so the run failed for an environment reason and
says nothing about the code. Re-run the spike with `--cc "clang -m32"` on a
machine with `gcc-multilib` installed before treating that option as viable.

## Standing recommendation

Keep the current host-decoding architecture for now — it is what renders
today — and treat this measurement as the input to the decision, not the
decision itself. The next thing worth measuring is the **link** surface: how
much of the Dolphin SDK the upstream `sysdolphin` sources actually reference,
and how much of that Aurora already provides. That number, not the syntax
number, determines how far a compiled-upstream path can be taken.
