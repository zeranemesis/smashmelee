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

`tools/upstream_native_spike.py` compiles every `.c` file under `src/melee`
and `src/sysdolphin` of an upstream checkout with the host compiler and host
libc, and groups the failures by cause.  `--link-surface` goes further: it
emits real object files, links their symbol tables together, and reports which
Dolphin SDK symbols remain unresolved and how many of those Aurora already
implements.

```sh
git clone --depth 1 https://github.com/doldecomp/melee.git /tmp/melee-upstream
tools/upstream_native_spike.py /tmp/melee-upstream
tools/upstream_native_spike.py /tmp/melee-upstream --link-surface
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

## The link surface

The same 940 units also produce real x86-64 object files, so the measurement
can be taken one step further:

```
940 objects define 24112 symbols and reference 11081 externals.
882 stay unresolved once they are linked together, 212 of them Dolphin SDK symbols.
Aurora implements 151 of those 212; 61 are missing:
    23  math: MTXFrustum MTXLightFrustum MTXOrtho MTXPerspective MTXRotRad ...
    21  OS: OSCreateThread OSCreateAlarm OSDisableInterrupts OSGetSoundMode ...
     7  VI: VIGetNextField VIGetRetraceCount VISetBlack VISetPostRetraceCallback ...
     6  GX: GXInitFogAdjTable GXNtsc480IntDf GXSetCopyClamp GXSetMisc GXSetTevClampMode GXWaitDrawDone
     3  cache: DCFlushRange DCInvalidateRange DCStoreRange
     1  PAD: PADSetSamplingRate
```

Sixty-one symbols is a list, not a project.  Most are shallow on a host: the
`MTX` projection helpers are textbook matrices, the `DC*` cache operations are
no-ops on a coherent host, and the `OS` thread and alarm surface maps onto the
host's own threading.  The rest of the 882 splits into roughly 520 symbols
that the 44 uncompiled files would themselves define, the musyx audio API
(`AX*`, `AXFX*` — already vendored as a submodule here), THP movie playback,
and a handful of libc functions.

One caveat on the Aurora figure: it is derived by scanning Aurora's sources
for function definitions, so it counts what is written, not what is verified
to behave like the console.

## What this does and does not show

It shows that the decompiled game's **types, headers, and declarations**
resolve against a host toolchain almost everywhere. The eight declaration
mismatches are upstream bugs that only a compiler stricter than MWCC reports,
and the two missing blobs are font data the decomp does not redistribute.

It does **not** show that the game runs.  Compiling and linking say nothing
about behavior: Aurora's GX has to draw what the console drew, the DAT
relocation model has to work, and byte order has to be handled at every read.
Those are the hard parts, and no symbol count measures them.

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
decision itself. The pointer-width fork above is the decision that has to be taken first,
because it determines whether archives can be relocated in place or must keep
being decoded into host structures — and therefore whether upstream's `gm`,
`sc`, and `mn` code can be compiled as-is or has to be adapted.

If the answer turns out to be "compile upstream", the 61 missing symbols are
the first piece of work, and they are small enough to schedule.
