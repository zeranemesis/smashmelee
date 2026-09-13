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
libc, and groups the failures by cause.

- `--aurora-headers` compiles against Aurora's Dolphin headers with
  `TARGET_PC`, which is the configuration a host port would use;
- `--only <path>` narrows the run to one subtree;
- `--link-surface` emits real object files, links their symbol tables
  together, and reports which Dolphin SDK symbols remain unresolved and how
  many of those Aurora already implements;
- `--fobj-reference` builds upstream's HSD core and runs its animation
  interpreter, printing the values `tests/hsd/test_fobj.cpp` asserts;
- `--pointer-casts` inventories every place upstream truncates a pointer
  through a 32-bit integer, reporting the units the port depends on
  separately from the rest.

```sh
git clone --depth 1 https://github.com/doldecomp/melee.git /tmp/melee-upstream
tools/upstream_native_spike.py /tmp/melee-upstream --aurora-headers
tools/upstream_native_spike.py /tmp/melee-upstream --aurora-headers \
        --only sysdolphin/baselib
tools/upstream_native_spike.py /tmp/melee-upstream --link-surface
tools/upstream_native_spike.py /tmp/melee-upstream --fobj-reference
tools/upstream_native_spike.py /tmp/melee-upstream --pointer-casts \
        --only sysdolphin/baselib
```

Since the submodule landed, `extern/melee` works in place of a separate
checkout.

It does not modify the checkout and copies no upstream code into this
repository. Two host shims are generated into a temporary directory that
shadows `src/Runtime/platform.h` on the include path:

- the upstream header declares `ssize_t` as `int`, which collides with the
  host libc declaration;
- `extern/dolphin/include/libc/math.h` defines `fabsf` as a macro, which
  rewrites the host `math.h` declaration of the same name.

Both are one-line accommodations a host port would carry permanently. A third
joined them once the Windows job reached `mtx.c` and `quatlib.c`: `M_PI` and
`M_PI_2` are not in standard C, and the two hosts disagree about how to ask
for them — MSVC wants `_USE_MATH_DEFINES` before `<math.h>`, glibc wants
`_DEFAULT_SOURCE`, which it has by default and loses under strict ISO. The
prelude asks both ways and then falls back to the literals, which are the same
in both libraries.

## Result

Two configurations were measured. The first uses upstream's own Dolphin
headers, which is the baseline; the second uses **Aurora's**, which is what a
host port would actually compile against, and is therefore the number that
matters.

Aurora's headers are not a drop-in: they stop at `Vec` and `S16Vec` where the
SDK also names `Vec2`, `Vec4`, `S8Vec3`, `U8Vec4`, `IntVec2`, `IntVec3`,
`S32Vec2`, `S32Vec3`, and they omit `GXTevClampMode`. Nine typedefs and one
enum, supplied by the spike's shim, are the whole difference between 12% and
93%.

```
# upstream's headers
940/984 (95.5%) ... but u32 is `unsigned long` there, so every integer field
                     doubles in width on LP64 -- compiling, not correct

# Aurora's headers, with TARGET_PC (tools/upstream_native_spike.py --aurora-headers)
918/984 (93.3%) whole game
 68/76  (89.5%) sysdolphin/baselib alone
```

The 66 remaining failures are a list, and this is all of it:

- 33 GameCube structure-layout assertions — the pointer-width fork, discussed
  below;
- 7 upstream declaration mismatches that only a compiler stricter than MWCC
  reports;
- 13 named gaps in Aurora's SDK surface: `GXSetTevClampMode`,
  `GXInitFogAdjTable`, `GXSetArray` (Aurora takes five arguments where the
  console SDK takes three — the two extra being a size bound and a
  little-endian flag, which is Aurora's own answer to byte order),
  `PADSetSamplingRate`, `CARDFormatAsync`, `VIPadFrameBufferWidth`, the
  `PAD_CONFIRM`/`PAD_CANCEL`/`PAD_STICK_*`/`PAD_ANY_LEFT`/`PAD_LR_START`
  constants, and the `state` and `fpscr` fields of `OSContext`;
- 2 font blobs the decompilation does not redistribute, and one Metrowerks
  libc internal in the debug console.

Every scene-object unit the port needs compiles: `jobj`, `dobj`, `mobj`,
`cobj`, `tobj`, `robj`, `aobj`, `fobj`, `objalloc`, `class`, `object`, `id`,
`list`, `mtx`, `archive`, `tev`, `texp`, `shadow`, `lobj`, `spline`,
`quatlib`, `random`, `util`, and `state`. `pobj` is the one that does not, on
the `GXSetArray` arity alone.

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

That 61 is an overcount, and the correction matters more than the number. It
was derived by scanning Aurora's `lib` sources for function definitions at
column zero, which cannot see a macro — and Aurora macro-aliases the
unprefixed SDK spellings onto its own implementations (`#define
MTXPerspective C_MTXPerspective`, `#define PSMTXIdentity MTXIdentity`).
Checking the same 61 against Aurora's **headers** instead puts 56 of them
within reach and leaves five genuinely absent: `GXInitFogAdjTable`,
`GXNtsc480IntDf`, `GXSetTevClampMode`, `GXWaitDrawDone`, and
`PADSetSamplingRate`.

Neither method is the answer. One under-reports because of macros; the other
over-reports, because a declaration in a header is not an implementation in a
library. Only a link settles it, and for the math group it now has: Aurora's
six matrix and vector units compile against their own headers with no further
dependency and resolve every math symbol upstream's scene units reference, as
`melee_hsd_upstream_tests` demonstrates by linking them.

The rest of the 882 splits into roughly 520 symbols that the 44 uncompiled
files would themselves define, the musyx audio API (`AX*`, `AXFX*` — already
vendored as a submodule here), THP movie playback, and a handful of libc
functions.

## Running upstream, and what it found

Upstream's HSD core does not merely compile: it runs. Built against Aurora's
Dolphin headers with `TARGET_PC` — which is what gives `u32` its host-correct
32-bit width; upstream's own headers declare it `unsigned long`, so every
structure doubles its integer fields on LP64 — nine translation units
(`objalloc`, `list`, `id`, `fobj`, `class`, `object`, `memory`, `hash`,
`spline`) link against exactly six host shims: `OSAllocFromHeap`,
`OSFreeToHeap`, `OSCheckHeap`, `OSReport`, `HSD_GetHeap`, and `__assert`.

Running the port's own assertions against that binary found a real defect in
this port. HSD packs an animation opcode and its key count into **one** byte —
`parseOpCode` peeks the low nibble without advancing, and `parsePackInfo`
then consumes the same byte for the count. The port was reading two separate
bytes, so every real animation stream decoded one byte out of step. Two
smaller divergences came out of the same comparison: the terminal state adds
back the segment length saved when a key rolled over rather than the length
parsed since, and reaching the end of a stream while loading a wait is the
terminal state rather than an error.

With those corrected, the two interpreters agree on every value across
twelve stream shapes — constant, linear, both spline forms, slope, key,
multi-byte waits, mixed packs, and three fraction encodings — and on the
state left behind. `--fobj-reference` regenerates that table, and
`tests/hsd/test_fobj.cpp` asserts it.

This is the argument for the compiled-upstream path stated as a fact rather
than a preference: a hand-written HSD is a second implementation that can be
wrong in ways nothing detects until a real asset looks subtly off.

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

This repository has already solved exactly that problem once, for Mario
Party 4. `include/port/byteswap.h` and `src/port/byteswap.cpp` — about 1,450
lines — carry the pattern:

- the decompiled game is compiled natively at **64 bits**;
- every on-disc structure gets a `<Name>32b` twin whose pointer fields are
  `u32`, matching the console layout exactly;
- a `byteswap_<name>()` converts a wire record into the host-width structure
  at load time, allocated from the game's own heap;
- the ~114 conversion sites in the game code sit behind `#ifdef BYTESWAPPING`.

`AnimData32b`, `HsfCluster32b`, and `HsfAttribute32b` are what that looks like
in practice. The game code above the conversion uses host-width pointers and
does not know the difference.

So the fork is narrower than it first appears. A 32-bit build would preserve
the struct layout, but byte order is still wrong — DAT data is big-endian —
so a conversion pass is needed either way. Given that, 32 bits buys layout
compatibility at the cost of a 32-bit host runtime on every shipping
platform, while the byteswap-and-widen route is already proven here.

A 32-bit measurement was attempted and is not reported: this container has no
32-bit libc headers, so the run failed for an environment reason and says
nothing about the code.

## Where the pointers actually truncate

The 33 structure-layout assertions say that something about the layout is
wrong on a 64-bit host; they do not say where. `--pointer-casts` does.

```
470 pointer-truncating casts across 23 of 76 translation units

In the 27 units the port depends on: 16 casts in 6 of them
     7  objalloc          3  jobj          2  pobj
     2  robj              1  aobj          1  archive
```

The 470 is dominated by subsystems the port does not reach: 282 in one
unidentified unit, 42 in the particle generator, 24 in the particle system,
21 in the debug console. Twenty of the 27 core units are completely clean,
`jobj` included, and the sixteen that remain are three patterns rather than
sixteen problems — the ID table keying runtime objects on a descriptor's
address, `GXSetArray`'s arity, and `Locate()`'s in-place relocation. They are
tabulated in [PLAN.md](PLAN.md) under phase 2.

This is the inventory to work from. It is also how phase 0 found that
`HSD_ObjSetHeap`'s arena holds its bounds as four `u32` fields and cannot be
used on a 64-bit host at all: the first upstream unit this repository
compiled segfaulted on exactly that, and the cast warnings named it.

## Sizing the conversion

A name-based scan of `sysdolphin/baselib`'s headers finds 31 `*Desc`
structures carrying 70 pointer fields between them, plus the handful of
on-disc types that do not use that suffix — `HSD_Joint`, `HSD_AnimJoint`,
`HSD_Image`, `HSD_Tlut`, `HSD_Material`. Call it on the order of 35
structures and 80 fields. Those are exactly the structures that need a
`*32b` twin and a converter, and Mario Party's equivalent came to 1,087 lines.

`HSD_Joint` is worth looking at, because it is the one the port already
decodes:

```c
typedef struct HSD_Joint {
    /* +0 */ char* class_name;      /* +14 */ Vec3 rotation;
    /* +4 */ u32 flags;             /* +20 */ Vec3 scale;
    /* +8 */ HSD_Joint* child;      /* +2C */ Vec3 position;
    /* +C */ HSD_Joint* next;       /* +38 */ MtxPtr mtx;
    /* +10 */ union { ... } u;      /* +3C */ HSD_RObjDesc* robjdesc;
} HSD_Joint;
```

Every offset there is one `HostScene::load_internal` already reads. The work
is not discovering the layout — that is done — it is expressing it as the
structure upstream's own code expects rather than as a new host type.

## Standing recommendation

Keep the current host-decoding architecture for now — it is what renders
today — and treat this measurement as the input to the decision, not the
decision itself. The pointer-width fork above is the decision that has to be taken first,
because it determines whether archives can be relocated in place or must keep
being decoded into host structures — and therefore whether upstream's `gm`,
`sc`, and `mn` code can be compiled as-is or has to be adapted.

If the answer turns out to be "compile upstream", the 61 missing symbols are
the first piece of work, and they are small enough to schedule.
