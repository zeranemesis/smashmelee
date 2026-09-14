# Plan: from here to a game that runs

This is the working plan for getting Super Smash Bros. Melee (GALE01 v1.02)
running natively, end to end. It is sequenced so that every step is
verifiable on its own, and it states what "done" means at each one.

Numbers quoted here were measured, not estimated; the method and the tooling
are in [UPSTREAM_NATIVE_SPIKE.md](UPSTREAM_NATIVE_SPIKE.md) and reproducible
through `tools/upstream_native_spike.py`.

## What "100%" means

The game boots from a legally obtained disc image, reaches every mode a
retail console reaches, plays them correctly at a deterministic 60 Hz with
sound, and saves. Concretely: 26 playable fighters plus the CPU-only ones,
every stage, items, single-player modes, menus, save data, replays, movies,
and audio.

That is roughly **481,000 lines** of already-decompiled C. This plan is not a
schedule; it is an order of operations. It is a multi-year effort at a
community's pace, and its value is that each phase ends somewhere useful
rather than somewhere half-finished.

## The decision this plan rests on

**Compile upstream's decompiled C natively at 64 bits, converting on-disc
structures at load time.** The port stops growing a second implementation of
HSD and starts compiling the original.

The evidence:

| Question | Measured answer |
|---|---|
| Does upstream C compile against Aurora's Dolphin headers? | 918 / 984 units (93.3%); 68 / 76 in `sysdolphin` |
| Does it run? | Yes — upstream's HSD core executes natively behind six host shims |
| How much SDK is missing from Aurora? | 61 symbols, plus ~13 named header gaps |
| How much conversion work? | ~35 on-disc structures, ~80 pointer fields |
| Has this repository done it before? | Yes — `src/port/byteswap.cpp`, 1,087 lines, for Mario Party 4 |

The alternative — continuing to reimplement HSD and the game by hand in C++ —
is the same 481,000 lines written twice, and this session already found three
defects that only existed because the second implementation could drift from
the first. A 32-bit build would preserve the console's struct layout, but DAT
data is big-endian either way, so a conversion pass is needed regardless; 32
bits only adds the cost of a 32-bit host runtime on every shipping platform.

**If that decision is reversed**, phases 1 and 2 change completely and
everything from phase 3 on grows by roughly the size of the module being
reimplemented. The rest of the sequencing still holds.

## Where the port is today

- A native shell mounts a GALE01 v1.02 image and exposes its filesystem.
- **35 of the 76 `sysdolphin/baselib` units** — upstream's own HSD, unmodified
  — compile, link and answer behavioral assertions: the object and class
  model, animation, the archive loader, and the entire render half (`jobj`,
  `dobj`, `mobj`, `pobj`, `tobj`, `cobj`, `lobj`, `tev`, `texp`, `state`,
  `shadow`, `robj`, `wobj`, `displayfunc`).
- **Twenty on-disc structures convert** from the console's layout into
  host-sized ones, and a converted model draws through upstream's own display
  path — a recorded 29-call GX frame, asserted.
- **The SDK callback boundary is decided and implemented**: one thread, an
  exact 675675-tick NTSC field, alarms at their own instant, interrupt masking
  that really defers a handler. Determinism is asserted.
- **120 test cases** (61 on the port's own HSD, 59 on upstream's) run in CI on
  GCC with sanitizers, Clang, and MSVC.
- The `bootstrap.cpp` menu flow is still hand-written, and
  `src/melee_port/hsd/` still holds 16 files the runtime uses.

**The game does not run.** What runs is its engine, on x86-64, fed by archives
in the disc's own layout, verified.

## What actually stands between here and a running game

The single most useful measurement of this session, for planning purposes:

```
tools/upstream_native_spike.py extern/melee --only melee --aurora-headers
→ 866 / 908 game translation units parse against Aurora's headers (95.4%)
```

That number was 850 an hour earlier, and the 16 it gained were not work — they
were a **measurement error this plan was repeating**. The spike tool carried
its own transcription of the build's prelude, and it had drifted: the build had
grown `_USE_MATH_DEFINES`, the `M_PI` fallbacks, the GX umbrella include, the
`GXSetArray` adapter and a dozen SDK spellings the tool never learned. It was
measuring a configuration nobody builds. It now force-includes the real
prelude, so there is one copy and the number means what it says.

The 42 that remain are a named list, and most of them are not blockers:

| Cause | Count | What it means |
|---|---|---|
| GameCube struct layout asserted | 32 | Not a blocker but the **inventory**: these are the `ASSERT_SIZE` lines that fail because a pointer is 8 bytes here. Each names a structure that needs converting, which is phase 2's work. |
| Declaration stricter than MWCC accepts | 8 | Prototype mismatches upstream's compiler tolerated. |
| `OSContext::fpscr` | 1 | Read by `debugconsole_main.c` alone, which does not ship. |
| One call arity | 1 | |

So the genuine remaining blockers number **ten**, and 32 of the 42 are a
to-do list phase 2 already has a mechanism for.

**Not one of the 42 is in `melee/ft`** — the fighters, 441 files and 145,000
lines, the largest subsystem in the game, now parse in full. The failures
cluster in `melee/gm` (19, the game manager and scene table) and `melee/gr`
(9, stages).

So the remaining work is not "compile the game". It is three things:

1. **Convert the rest of the on-disc structures.** Twenty done, roughly
   fifteen to go for the engine, plus whatever the 32 asserting units name.
2. **Fill the SDK gaps.** Done for everything named except audio — and none of
   it had to be invented: every spelling came from upstream's own bundled SDK
   headers, which sit behind Aurora's on the include path and were shadowed
   rather than missing.
3. **Make it run** — boot order, the frame loop, and the scene flow.

### The one genuinely unmeasured gap: audio

Aurora implements `AR`, `CARD`, `DVD`, `GX`, `MTX`, `OS`, `PAD`, `SI` and
`VI`. It implements **no `AX`, `AI`, `DSP` or `THP`** — headers only. Melee's
audio path is HAL's own `axdriver.c` and `synth.c` (both decompiled, both in
`sysdolphin`) calling **60 distinct `AX` symbols**: voice acquisition, ADPCM
streaming, mixing, and the reverb, chorus and delay effects.

`extern/musyx` is vendored, but **Melee does not use MusyX** — zero references
in the whole decompilation. It belongs to the other port sharing this
repository. Phase 6 therefore means implementing a GameCube DSP voice mixer,
or bridging those 60 symbols onto a host audio library. That is the part of
this plan with no measurement behind its estimate.

---

## Phase 0 — Make upstream source available to the build · **done**

**Objective.** The build can compile upstream C without anyone copying it in
by hand.

1. Add `doldecomp/melee` as a pinned submodule under `extern/melee`. No
   upstream code is committed to this repository; only the pin moves.
2. Add a `melee_upstream` CMake interface target carrying the include paths,
   `TARGET_PC`, and the compatibility prelude.
3. Move the spike's shim into the build as
   `include/melee/port/dolphin_compat.h`: the SDK vector typedefs Aurora
   lacks (`Vec2`, `Vec4`, `S8Vec3`, `U8Vec4`, `IntVec2`, `IntVec3`,
   `S32Vec2`, `S32Vec3`), `GXTevClampMode`, `BOOL`/`TRUE`/`FALSE`, and the
   `ssize_t` and `fabsf` accommodations.
4. Teach the offline suite to build upstream units, so a ported unit and its
   replacement can be compared in the same binary.

**Done.** `doldecomp/melee` is pinned at `extern/melee`,
`include/melee/port/dolphin_compat.h` carries the SDK spellings Aurora lacks,
`cmake/MeleeUpstream.cmake` generates the `Runtime/platform.h` shadow and
configures a target, and `melee_hsd_upstream_tests` compiles upstream's
`objalloc.c` and `memory.c` and drives them through the same behavioral
assertions the port's allocator answers. Both binaries run under `ctest`, on
three toolchains.

Two findings came out of getting it to run, and both change phase 2:

- **`HSD_ObjSetHeap`'s arena cannot be used on a 64-bit host.** `objheap`
  holds its bounds as four `u32` fields, so `obj_heap.curr = (u32) ptr`
  truncates the address and the first allocation dereferences garbage.
  Leaving the arena unset takes the other path in `HSD_ObjAllocAddFree`,
  which calls `HSD_MemAlloc` and keeps a real `void*`. A host build has to
  take that path.
- **`HSD_ObjAlloc` refills one object at a time**, so pool slots are not
  contiguous the way the port's block allocator makes them. Nothing depends
  on that, but a test that assumes adjacency will fail.

## Phase 1 — Close the Dolphin SDK boundary

**Objective.** Every symbol upstream references either exists or is
deliberately stubbed, with the stub's consequence written down.

The gap here is far smaller than the first measurement suggested, and the
correction is worth stating plainly. That measurement counted a symbol as
missing unless Aurora's `lib` sources *defined* it at column zero — which
cannot see a macro. Aurora macro-aliases the unprefixed SDK spellings onto its
own implementations (`#define MTXPerspective C_MTXPerspective`, `#define
PSMTXIdentity MTXIdentity`), so 56 of the 61 are in fact reachable. **Five are
genuinely absent from the headers:**

| Symbol | Kind |
|---|---|
| `GXInitFogAdjTable` | fog table setup |
| `GXNtsc480IntDf` | the NTSC render-mode object, data rather than a function |
| `GXSetTevClampMode` | TEV state |
| `GXWaitDrawDone` | draw synchronisation |
| `PADSetSamplingRate` | controller polling rate |

Neither number is the last word: the first under-reported because of macros,
and "declared in a header" is not "implemented in a library" — only a link
settles it. For the **math** group it has been settled. Aurora's six matrix
and vector units compile against their own headers with no further
dependency, and together they resolve every one of the 22 math symbols
`mtx`, `cobj`, `jobj`, `dobj`, `tobj`, `robj` and `lobj` reference. The 23
helpers this plan expected to write did not need writing.

### The threading model · **settled, and smaller than this plan thought**

This plan called the threading model "the only genuinely design-sensitive
piece" and framed it as coroutines over `libco` versus preemptive host
threads. Measuring the decompilation dissolved the question.

**Melee's shipping code creates no OS threads.** `OSThread` appears four times
in 1034 files: once in `debugconsole_main.c`, and the rest in `MetroTRK`, the
Metrowerks debugger stub. Neither ships. The SDK itself is not decompiled at
all — `src/` holds `MSL`, `MetroTRK`, `Runtime`, `melee` and `sysdolphin`, and
no `dolphin` — so DVD and audio threading lives in libraries this port does
not compile. That is Aurora's problem, not the game loop's.

What the game does use is three things, and they are all about *when a
callback is allowed to run*:

| Surface | Calls | What it is really for |
|---|---|---|
| `OSDisableInterrupts` / `OSRestoreInterrupts` | 225, in 18 files | Guarding a structure against a **handler**, not a thread. `video.c` masks interrupts to swap a retrace callback pointer without the retrace handler seeing it half-written. |
| `OSAlarm` | 3 users | Timers. `lbmemory.c` is the clearest: copy `0x19000` bytes, re-arm a 3 ms alarm, copy the next chunk — cooperative time-slicing written by hand, which is the closest thing in the game to a background thread and the best argument for not building one. |
| SDK completion callbacks | — | DVD reads and audio, arriving from libraries this port does not compile. |

So the model is **one thread**, and the only decision worth making is where
callbacks are delivered. `src/melee_port/upstream/os_scheduler.cpp` makes it:
a timebase that advances one exact NTSC field at a time, alarms that fire at
their own instant in time-then-arming order, and a mask that really does hold
a handler off until it is lifted.

Two properties are worth recording:

- **the NTSC field is an exact number of ticks.** 40.5 MHz over 60000/1001 Hz
  is 675675 with no remainder, so a fixed step accumulates no error and the
  port can be exact about time without a rational accumulator;
- **a deferred handler sees a late clock, deliberately.** The decrementer
  exception was pending while the mask was up and is taken when it comes down,
  so `OSGetTime()` reports the moment of the restore, not the moment the alarm
  was set for. A port that rewound the clock there would be inventing an
  accuracy the hardware never had. A handler that needs the scheduled instant
  reads it off `alarm->fire`, which survives.

Determinism is asserted rather than hoped for: two runs of the same schedule
produce the same handler calls at the same timebase values.

`libco` stays vendored and unused. If a future need for real coroutines
appears — a blocking DVD read the game expects to yield on — it is there, and
the decision can be revisited against a concrete case rather than a guess.

### The absent symbols · **supplied, and none of them invented**

All five, plus the `PAD_*` aliases and two more the game needed, turned out to
be **sourceable from upstream's own bundled SDK headers** under
`extern/melee/extern/dolphin/include` — which sit *last* on the include path,
behind Aurora's, so the declarations were shadowed rather than missing. The
prelude states them with a comment naming where each came from.

`GXNtsc480IntDf` is the one exception, because it is Nintendo's *data* rather
than a declaration, and the split matters:

- the **geometry** is unambiguous NTSC 480i, and is what the game computes
  with — `efbHeight` read twelve times across the decompilation, `fbWidth`
  nine, then `xfbHeight`, `viWidth`, `viHeight`, `field_rendering` and `aa`.
  All 640×480, no field rendering, no antialiasing;
- the two **filter tables** are read exactly once each, only to hand straight
  to `GXSetCopyFilter`. The vertical filter the port supplies is the flat
  kernel (21, 22, 21 over seven taps, summing to 64). The deflicker kernel the
  `Df` in the name refers to is **not** reproduced, because its coefficients
  are in neither tree. The consequence is a sharper image than the console's,
  not a wrong one.

`fog` and `video` compile and link on the back of that, which leaves **two**
units outside the conformance target rather than four: `psdisp`, which asserts
a pointer-holding structure is eight bytes, and `debug`, which is written
against the Metrowerks libc's `FILE`. `OSContext::fpscr` turned out to matter
to exactly one unit — `debugconsole_main.c`, which does not ship.

### VI, and the frame boundary · **bridged**

`src/melee_port/upstream/vi_bridge.cpp` supplies the ten `VI` entry points
`video.c` needs, and the shape of it is the threading finding paying off.

`VIWaitForRetrace()` is the blocking wait a threaded port would have needed a
coroutine for. Melee calls it inside loops that spin until a framebuffer frees
up:

```c
while ((idx = HSD_VIGetXFBDrawEnable()) == -1) { VIWaitForRetrace(); }
```

With no other thread to yield to, that wait is not a yield — it is the step
that moves the world forward. So it advances the timebase by one exact field,
fires every alarm that comes due inside it, and then delivers the retrace.
**The game's own main loop is the frame pump**, which is the arrangement the
console had.

Retrace goes through the scheduler rather than being called directly, because
retrace *is* an interrupt: `video.c` masks interrupts to swap the callback
pointers, and that critical section has to hold the handler off. It does, and
there is a test for it. Upstream's own `HSD_VIInit` now runs — it configures
the mode, registers its callbacks and its draw-done callback, sets up two
external framebuffers — and a field passes with its callbacks firing.

### What remains for this phase

- the `OS` arena and heap, which `initialize.c` reaches for — the last thing
  between the conformance target and upstream's own `HSD_InitComponent`;
- the `DC`/`IC` cache operations, which are no-ops on a coherent host;
- ~~`GXSetArray`'s two extra arguments.~~ **Done.** The adapter is in the
  prelude, and `src/melee_port/upstream/gx_array_registry.cpp` answers both
  from what the load recorded: the extent from the archive the vertex array
  lives in, and the byte order from whether anything converted it. Nothing
  converts vertex data today, so the answer is the console's byte order — and
  a recorded trace shows it.

**Done when** upstream's `sysdolphin/baselib` links with no unresolved
symbol, and each stub is listed in a table with what it does not do.

**Size.** Smaller than first estimated twice over: the math needed no writing,
and the threading model turned out not to be a threading model.

## Phase 2 — Swap the hand-written HSD for upstream's

**Objective.** `src/melee_port/hsd/` is gone, and upstream's
`sysdolphin/baselib` is doing the work, with the offline suite unchanged.

1. Convert the ~35 on-disc structures into host-sized ones. The existing
   decoders in `scene.cpp` and `animation.cpp` are the specification: every
   field offset they read is already correct and already covered by tests.
   **This does not take the shape this plan first expected** — see *Why there
   are no wire structs* below.
2. Replace unit by unit, keeping the suite green at each step:
   `objalloc` → `class`/`object` → `list`/`id` → `mtx` → `fobj`/`aobj` →
   `jobj` → `dobj`/`mobj`/`tobj`/`pobj` → `cobj`/`lobj`/`fog` → `robj` →
   `tev`/`texp` → `shadow`.
3. Retire each host decoder only once its upstream replacement passes the
   cases that pinned it.

The suite already pins `objalloc`, `class`/`object`, `list`, `id`, `fobj`
(twelve stream shapes) and `aobj` (seven playback modes) against upstream's
own behavior, so most of this phase has its acceptance test written already.

**Thirty-five units have moved**, which is the whole of HSD's scene graph and
most of what stands under it: the object and class model (`objalloc`, `class`,
`object`, `list`, `id`, `hash`, `memory`), animation (`fobj`, `aobj`,
`bytecode`, `spline`), the archive loader, the math pools (`mtx`, `quatlib`)
and utilities (`util`, `random`), the entire render half (`jobj`, `dobj`,
`mobj`, `pobj`, `tobj`, `cobj`, `lobj`, `tev`, `texp`, `texpdag`, `state`,
`shadow`, `robj`, `wobj`, `displayfunc`), and the particle system that owns
the skinning helper the scene graph builds envelope matrices with (`particle`,
`generator`, `psappsrt`, `perf`). Aurora's matrix and vector implementations
link in alongside.

Running them side by side found a real defect in the port's `ref_DEC`, which
released a reference one call early whenever more than one was held, and
established that the port's `hsdSearchClassInfo` is a host addition —
upstream's reads a hash nothing populates and always answers NULL. `fobj`
carries the twelve-stream table, now asserted against both interpreters, so
the encoding bug that started this cannot come back.

**Four units remain outside, each for a named reason.** `fog` needs
`GXInitFogAdjTable`; `video` needs `VIPadFrameBufferWidth`, `GXWaitDrawDone`
and the `GXNtsc480IntDf` render mode — none of the four are in Aurora, and
they are four of the five absent SDK symbols phase 1 lists. `psdisp` asserts
that a structure holding a pointer is eight bytes, which it is not on a
64-bit host; that one is a `*32b` twin, not a missing symbol. `debug` is
written against the Metrowerks libc's `FILE` internals, so a host port
replaces it rather than compiles it. `initialize` is not a gap but a phase:
it reaches for the OS arena and heap, which is phase 1's work, and until it
arrives the suite supplies the two things it owns — `HSD_GetCurrentRenderPass`
and, from `video`, `HSD_VIData` — from `tests/hsd/upstream_host.c`. Both are
written to collide deliberately: the day those units join, the linker says so.

### Why there are no wire structs

The plan called for `<Name>32b` structs laid over the archive's bytes with
big-endian fields, and `byteswap_<name>()` copying them into the host
structure, following `src/port/byteswap.cpp`. For Melee's containers that
shape cannot express the data.

**An HSD pointer field is not self-describing.** The console's loader adds the
data section's base to every field named in the archive's *relocation table*
and leaves every other field alone. A pointer field holding zero is therefore
the first byte of the data section when the table names it, and NULL when it
does not — identical in the struct, different only in a table stored
elsewhere in the file. A wire struct cannot tell them apart. This port already
had that defect once and fixed it, which is what `Archive::kNullOffset`
exists for.

So `src/melee_port/upstream/archive_convert.cpp` reads through `Archive`,
which carries the relocation table and answers `kNullOffset` for a field the
table does not name. The bounds checks come along for free.

**The ID-table truncation is fixed, and the fix is worth stating.** Upstream
keys its ID table on `(u32) joint` — the low word of a joint descriptor's
address. On a 64-bit host two descriptors four gigabytes apart share a key,
and the table answers with whichever was registered last: not a crash, but a
constraint or a skinning weight quietly following the wrong bone.
`tests/hsd/test_upstream_core.cpp` demonstrates it with two fabricated
addresses. The port cannot change the key without changing upstream, so it
changes where joints live: one allocation whose size is a power of two and
whose base is aligned to that size cannot straddle a boundary its own size, so
every low word in it is distinct. One allocation, not a chain — two separately
aligned blocks could collide with each other, so running out is a refusal with
a reason. `HSD_Joint` is the only structure that needs this; it is the only
one whose address upstream ever truncates.

**Twenty structures are converted**: `HSD_Joint`, `HSD_DObjDesc`,
`HSD_MObjDesc`, `HSD_Material`, `HSD_PEDesc`, `HSD_PObjDesc`,
`HSD_VtxDescList`, `HSD_ShapeSetDesc`, `HSD_EnvelopeDesc`, `HSD_TObjDesc`,
`HSD_ImageDesc`, `HSD_TlutDesc`, `HSD_TexLODDesc`, `HSD_TObjTevDesc`,
`HSD_RObjDesc`, `HSD_IKHintDesc`, `HSD_ExpDesc`, `HSD_ByteCodeExpDesc` and
`HSD_RvalueList` — a whole textured, skinned, constrained model. A material converts entire: `renderdesc` is
the only field left null, and deliberately, because it appears exactly once in
upstream's tree — its own declaration — so nothing reads it.

The skinning path is where the relocation-table rule earns its keep twice
over. A primitive flagged `POBJ_ENVELOPE` points at a NULL-terminated array of
runs, and each run is `{joint, weight}` pairs ending at an entry whose joint is
NULL. Both terminators are pointer fields the relocation table does not name —
a run ended by a *relocated* zero is a run whose last entry weights the joint
at offset zero, and nothing in the bytes tells the two apart. Converting the
envelope also makes the joint conversion re-entrant: a skinned primitive names
joints, and it is reached from inside its own joint's conversion. Every joint
is registered before its fields are filled in, so a nested call finds an entry
rather than building a second copy or recurring forever. Raw data that is not a structure — a
vertex array, a display list, a string, a matrix — has no pointers in it and
no size change on a 64-bit host, so it stays in the archive and is addressed
where it lies, through `Archive::data_span()`.

Three properties are load-bearing rather than incidental:

- **one on-disc structure gets exactly one host address.** HSD keys its ID
  table on a descriptor's address and reference-counts by identity, so a joint
  reached through two parents must not become two host objects;
- **what it cannot build yet is recorded, not silently nulled.** A texture, a
  spline, a particle list or a pixel-engine descriptor is reported through
  `unconverted()` with the offset and the kind. A null texture draws
  untextured and looks exactly like a broken renderer; a list of them looks
  like what it is;
- **vertex arrays keep the console's byte order, and say so.**
  `src/melee_port/upstream/gx_array_registry.cpp` records each array's extent
  and byte order at load, which is what `GXSetArray`'s two extra arguments are
  answered from. That closes the last item phase 1 left open on the SDK
  boundary.

### The first frame

The test that matters is `UpstreamConvert.DrawsAConvertedModelThroughTheGamesOwnDisplayPath`.
An archive in the console's layout goes in; upstream's `jobj`, `dobj`, `mobj`
and `pobj` walk it and talk to GX; the recorder writes down what they said:

```
GXPixModeSync GXSetTevKColor GXSetTevColor GXPixModeSync GXSetTevOrder
GXSetTevColorOp GXSetTevColorIn GXSetTevAlphaOp GXSetTevAlphaIn
GXSetTevSwapMode GXSetTevKColorSel GXSetTevKAlphaSel GXSetColorUpdate
GXSetBlendMode GXSetZMode GXSetZCompLoc GXSetAlphaCompare GXSetNumTevStages
GXSetNumTexGens GXSetNumChans GXSetChanMatColor GXSetChanCtrl
GXSetCurrentMtx GXLoadPosMtxImm GXSetArray GXClearVtxDesc GXSetVtxDesc
GXSetVtxAttrFmt GXCallDisplayList
```

Twenty-nine calls: a TEV stage, the pixel-engine state, one lighting channel,
the position matrix, the vertex binding and the draw. Nothing in it is a
reimplementation. That sequence is asserted, so a change to the converter, the
loader or the SDK boundary that moves any of it says so. It is also the
mechanism phase 3 needs, arriving early — what remains for phase 3 is a real
GX behind it and a window to put the result in.

Adding the texture put a second frame beside the first, and the difference
between them is the whole texture path:

```
GXLoadTexMtxImm GXInitTexObj GXInitTexObjLOD GXLoadTexObj GXSetTexCoordGen2
GXPixModeSync GXSetTevKColor GXSetTevColor GXPixModeSync GXSetTevOrder
...
GXSetArray GXClearVtxDesc GXSetVtxDesc GXSetVtxAttrFmt GXCallDisplayList
```

The texture matrix, the texture object built from the image's own dimensions
and format, the LOD state, the bind, and a generated texture coordinate — none
of which the untextured frame had. Upstream's `tobj.c` also answered a
question about the format along the way, by asserting: a texture whose
`repeat_s` or `repeat_t` is zero is malformed, not a texture with no repeats,
because `MakeTextureMtx` divides by them.

That is the mechanism the remaining ~15 structures follow.

**A function pointer in the archive.** `HSD_ExpDesc` holds
`f32 (*func)(void*)` — on the console, the address of a routine in the
executable. There is no host value that means the same thing, and a truncated
one would be a jump into nothing. The converter leaves it NULL *deliberately*,
because upstream already handles that: `expLoadDesc` substitutes `dummy_func`.
The safe answer turned out to be upstream's own answer.

What `unconverted()` still reports, and therefore what remains before a real
archive converts whole: `HSD_Spline` or the particle `HSD_SList` where a
joint's union holds one of those instead of a display object, and an
`HSD_RObjDesc` whose type is none of the five `HSD_RObjLoadDesc` handles —
which upstream panics on, so reporting it rather than guessing is the point.

### What the pointer work actually is

`tools/upstream_native_spike.py --pointer-casts` inventories every place
upstream truncates a pointer through a 32-bit integer. Across
`sysdolphin/baselib` there are 470 of them, but they are concentrated in
subsystems the port does not reach — 282 in one unidentified unit, 42 in the
particle generator, 21 in the debug console. **In the 27 units the port
depends on there are 16, in 6 units**, and they fall into three patterns:

| Pattern | Sites | What it is |
|---|---|---|
| ID-table keys | 9 (`jobj`, `pobj`, `robj`, `aobj`) | **Resolved, and not the way this table first proposed.** HSD stores a joint descriptor's **address** as the hash key — `HSD_IDInsertToTable(NULL, (u32) joint, jobj)` — and five places look one back up the same way. Changing the key would mean changing upstream. Changing where the descriptors live does not: `src/melee_port/upstream/descriptor_arena.cpp` allocates them from one block whose size is a power of two and whose base is aligned to that size, and such a block cannot straddle a boundary its own size — so the low word of every address in it is distinct by construction. |
| `GXSetArray` arity | 2 (`pobj`) | **Resolved.** Aurora's `TARGET_PC` form takes the array's byte length and its byte order too, because it writes a 64-bit base into the command stream and the backend copies the array out rather than reading it where it lies. `include/melee/port/dolphin_compat.h` maps the console's three arguments onto Aurora's five; the two the call site cannot supply are asked of `melee_gx_array_extent()` and `melee_gx_array_is_little_endian()`, which is where phase 3 owes a real answer. |
| In-place relocation | 1 (`archive`) | `Locate()`'s `*ptr += (u32) archive->data`. This is the one the `*32b` converters replace. |
| Pool arena | 7 (`objalloc`) | Avoided entirely by leaving `HSD_ObjSetHeap` unset, as phase 0 found. |

Twenty of the 27 core units are completely pointer-clean, `jobj` included.

### The gate the rest of the layer waited on · **cleared**

The scene-object layer was never held back by the pointer casts. It was held
back by GX: every unit in the render half calls it, and Aurora's
implementation pulls in the window, the swapchain and the shader compiler.

`tests/hsd/gx_record.cpp` clears it with **84 GX entry points**. Each is
defined `extern "C"` against Aurora's own declaration, so a signature that
drifts from the real GX does not compile — the same reason the `*32b`
converters come from upstream's code rather than from a document.

Eighty-one of them write down what they were told instead of drawing. The
other three are the `GXGetTexObj*` getters, which answer from what
`GXInitTexObj` stored in the texture object, because HSD reads its texture
dimensions back out of GX rather than keeping its own copy and branches on
what it gets. `GXGetTexBufferSize` is the fourth of that kind and already
existed: `tests/hsd/gx_texture_stub.cpp` mirrors the console's tile
arithmetic, and both targets share it.

The trace is deliberately comparable rather than merely inspectable. Pointers
never reach it as addresses — each distinct pointer takes a small index in the
order the trace first sees it — so the same scene produces the same text on
every host, under any allocator. The three matrix entry points record the
matrix *elements*, not the address of the matrix, because a trace that says a
camera loaded some matrix is worth nothing.

That is not a placeholder for a renderer. It is the instrument phase 3 needs,
and it already works: setting a camera current records

```
GXSetViewport(0, 0, 640, 480, 0, 1)
GXSetScissor(0, 0, 640, 480)
GXSetProjection([1.29904 0 0 0 0 1.73205 0 0 0 0 -0.010101 -1.0101 0 0 -1 0], 0)
```

— three calls and nothing else, with a projection matrix whose terms are the
console's own: the near plane over the half-width and half-height it subtends,
and a depth range mapped into [0, -1] with w carried in the last row. The
suite asserts the shape of that frame as text and its computed floats with a
tolerance, which is how a golden frame has to be compared when the numbers
come out of the host's `tanf`.

One more thing is now asserted rather than assumed: upstream's
`HSD_ArchiveParse` refuses a big-endian container on a little-endian host —
it compares the file size it reads against the one it was given, and reports
`byte-order mismatch` in its own words. That is the converters' justification
coming from upstream's code rather than from this document.

**Done when** no file remains under `src/melee_port/hsd/`, and all 61 cases
plus whatever the swap adds are green on three toolchains.

**Size.** Large but bounded: ~35 converters, ~76 units to bring up, and 14
pointer casts left to resolve in the units that matter — `GXSetArray`'s two
are done.

## Phase 3 — The first frame drawn by the game's own code

**Objective.** The geometry the port renders today is rendered by upstream's
`HSD_JObjDisp`, not by `MeleeSceneRenderer`.

1. Host `HSD_InitComponent`: OS, VI, GX FIFO, `HSD_ObjInit`, ID table.
   `initialize_host_runtime()` is already the shape of this.
2. `HSD_ArchiveParse` over a byteswapped, converted `MnMaAll.dat`.
3. `HSD_JObjLoadJoint` on `MenMainBack_Top_joint`, `HSD_CObjSetCurrent` on
   `ScMenMain_cam_int1_camera`, `HSD_JObjDisp` into Aurora GX.

**Done when** a frame of `MenMainBack` drawn by upstream's renderer matches
the current one — 86 draw objects, 324 triangles — and the hand-written
renderer is deleted.

**Size.** Medium. This is the phase that proves the whole approach, and the
first place Aurora's GX fidelity is really tested.

## Phase 4 — The game drives itself

**Objective.** `bootstrap.cpp`'s hand-written menu state machine is deleted,
and Melee's own scene flow runs.

Port order follows the dependency closure, not the call order:
`melee/lb` (17k lines) → `melee/db` (2k) → `melee/sc` types →
`melee/gm` (55k, the scene table and game manager) → `melee/mn` (33k, menus).

`gmmain.c`'s initialization order is already transcribed in
[MELEE_PORT.md](MELEE_PORT.md). Files come from the mounted disc through the
existing `disc_mount` layer over Aurora DVD.

**Done when** `gm_801A4510` runs the boot sequence, the title screen and main
menu respond to a controller, and no menu logic remains in this repository.

**Size.** Large but measured. ~107,000 lines across 155 translation units, and
117 of them already parse — the 38 that do not are concentrated: 27 in
`melee/gm`, 5 each in `melee/lb` and `melee/mn`, 1 in `melee/db`.
`melee/gm` is the densest cluster of remaining work in the whole game, which
is unsurprising: it is the scene table, and the scene table names everything.

## Phase 5 — A match

**Objective.** Two fighters, one stage, a complete versus match.

`melee/pl` (players, 6k) → `melee/cm` (camera, 5k) → `melee/if` (HUD, 10k) →
`melee/ft` common (the shared fighter engine) → one fighter → `melee/gr` one
stage → `melee/it` minimal (items off).

**Done when** a two-player match starts from the character select, plays to a
result screen, and replays identically from the same inputs.

**Size.** Large in line count, small in unknowns. `melee/ft` is 441 files and
145,000 lines, and **440 of them already parse** — one failure in the whole
subsystem. `ft/kinds/ftCommon` is the shared engine; a single fighter on top
of it is comparatively small. The risk here is not compilation, it is that
fighter behavior is where a wrong conversion first becomes visible as a
gameplay difference rather than a crash.

## Phase 6 — Sound

**Objective.** Music and effects.

**The estimate here was wrong and is now corrected.** This plan said "musyx is
already a submodule", implying the engine was in hand. It is not: `extern/musyx`
is vendored for the *other* port in this repository, and **Melee references
MusyX nowhere** — zero hits across 1034 files.

Melee's audio is HAL's own. `sysdolphin/baselib/axdriver.c` and `synth.c` are
decompiled and present, and they call **60 distinct `AX` symbols**: voice
acquisition and release, ADPCM streaming with loop points, per-voice mixing
and volume envelopes, sample-rate conversion, and the four effect chains
(reverb standard, reverb high, chorus, delay). Aurora implements none of
them — `ai.h`, `dsp.h` and `thp.h` are headers with nothing behind them.

So this phase is one of:

- implement the GameCube DSP voice mixer those 60 symbols describe, against a
  host audio callback; or
- bridge them onto an existing implementation (Dolphin's DSP-HLE is the
  reference, and its licence and shape would have to be examined first).

**Done when** menu music, stage music, and hit effects play in sync with the
simulation.

**Size.** Medium to large, and **the only part of this plan with no
measurement behind its estimate**. It should be spiked before it is
scheduled: implement `AXAcquireVoice`, `AXSetVoiceAddr`, `AXSetVoiceAdpcm`,
`AXSetVoiceVe` and `AXSetVoiceMix` well enough to play one sound effect from
one Melee bank, and let that tell the real number.

## Phase 7 — Everything else

- The remaining fighters (35 kinds, 145k lines), all stages (77 files, 56k),
  the full item set (183 files, 74k).
- Single-player: Classic, Adventure, All-Star, Event matches, Stadium,
  Training, Tournament, Special Melee.
- `melee/ty` trophies (12k), `melee/mp` (12k), `melee/ef` effects (4k),
  `melee/vi` video (2.5k).
- Save data through CARD, replays, THP movie playback.

**Done when** a mode checklist is complete and each entry has been played.

**Size.** The bulk of the remaining line count, but the most parallelizable:
fighters and stages are largely independent once the engine is up, and both
already compile — `melee/ft` fails on one unit of 441, `melee/gr` on nine of
77, `melee/it` on none of 183.

## Phase 8 — Fidelity and shipping

- Frame-level comparison against Dolphin on fixed inputs.
- Input latency, rumble, controller configuration.
- Performance on the target platforms.
- Packaging for Windows, Linux, and macOS through the existing CI.

---

## Cross-cutting work

**Testing.** Every phase extends `tests/hsd`. The differential technique —
running the port's assertions against upstream's own code — found all three
defects fixed this session and should be the default for anything ported by
hand. `tools/upstream_native_spike.py --fobj-reference` is the template.

**Determinism.** The fixed 60 Hz step already exists. It must survive the
threading model chosen in phase 1; this is the main reason to prefer
coroutines over host threads.

**Golden frames.** From phase 3 on, a headless render of a known scene,
compared against a stored image, catches GX regressions that no unit test
will. The call-stream half of that already exists — `tests/hsd/gx_record.cpp`
— and running it found the first thing a golden frame has to be built around:

> `HSD_TExpSetReg` in `texp.c` declares `GXColor reg[8]`, never initializes
> it, and writes only the components a constant names before handing the whole
> colour to GX. So `GXSetTevKColor`'s alpha and `GXSetTevColor`'s rgb are read
> before they are written — **in the shipped game**, not in this port.
> Compiling upstream with `-ftrivial-auto-var-init=pattern` turns both into
> `0xAA`, which is how this was established rather than guessed.

A golden frame therefore cannot pin those components on any host, and a
recorded trace of a textured draw is only deterministic with them excluded.
`-ftrivial-auto-var-init=zero` would make them deterministic on GCC and Clang,
but MSVC has no public equivalent, so relying on it would give confidence on
two toolchains out of three. The comparison excludes them instead, and says
why where it does it.

**Assets stay out.** No ISO, no `main.dol`, no game data in this repository,
ever. A legally obtained disc image is a runtime input.

## Risks, in order of how much they could cost

1. **Aurora's GX does not draw what the console drew.** TEV chains,
   fog, lighting, and shadows are where this shows. Mitigation: golden frames
   from phase 3 onward, so divergence is caught on the first scene rather
   than during fighter bring-up. The recorder makes this cheap and it already
   works; what it also showed is that some of what the console drew was never
   determinate in the first place — see *Golden frames* above.
2. **Audio, and it is now the top unmeasured risk.** Aurora implements no
   `AX`, `AI`, `DSP` or `THP` — headers only — and Melee's decompiled audio
   driver calls 60 distinct `AX` symbols. The mitigation this plan used to
   name was wrong: `extern/musyx` belongs to the other port in this
   repository and Melee references it nowhere. Mitigation: spike five `AX`
   voice calls against one Melee sound bank and let that produce the estimate,
   before phase 6 is scheduled at all.
3. ~~**The threading model.**~~ **Retired.** The risk assumed a choice that
   does not exist: the game creates no threads, so there is no scheduling for
   the rest of the port to inherit. What replaced it is a callback scheduler
   whose determinism is asserted. The residual risk moved to phase 6: audio
   *does* run on a thread inside the SDK, and that thread is Aurora's.
4. **A conversion that is wrong but not fatal.** Twenty structures convert and
   each is tested, but a field read from the wrong offset produces a model
   that draws — slightly wrong. The fighters are where this first becomes
   visible, and by then it is 145,000 lines away from the cause. Mitigation:
   every converter asserts against upstream's own consumer, not against a
   document, and the 32 units whose layout assertions fail are a list of
   exactly which structures still have no twin.
5. **The 33 structure-layout assertions.** They exist to verify the match
   against the original binary and are meaningless on a 64-bit host.
   Mitigation: disable for host builds, and record that the `*32b` twins are
   what now guarantees the layout. They are also the wrong inventory to work
   from — `--pointer-casts` names the actual sites, and 14 remain in the units
   that matter. One assertion is not merely noise, though: `psdisp` asserts a
   pointer-holding structure is eight bytes, and that is a real `*32b` twin
   waiting to be written rather than an assertion to switch off.
6. **Upstream drift.** The submodule is pinned; updating it is a deliberate
   act with the suite as the gate.
7. **Scope.** 481,000 lines is a multi-year effort at a community's pace. The
   mitigation is the sequencing itself: every phase above ends somewhere
   playable or measurable, so the work has value before it is finished.

## The next five actions

Phase 0 is done, most of phase 1 is done, and phase 2's mechanism is built.
These are what follow, in order.

1. **Bring `initialize.c` over** (phase 1) — the `OS` arena and heap surface it
   reaches for is the last thing between the conformance target and upstream's
   own `HSD_InitComponent`, which is what phase 3 has to call.
2. **Convert `HSD_Spline` and the particle list** (phase 2) — the last two
   things `unconverted()` reports, after which a whole archive converts.
3. **Re-run the game-wide syntax check** and confirm the 26 units the new
   declarations should have cleared actually cleared.
4. **Load `MnMaAll.dat` from the disc and draw it with upstream's renderer**
   (phase 3). Everything it needs now exists: the archive reader, the
   converters, the scene graph, the camera, and the recorder to compare
   against. This is the step that proves the approach end to end, and it is
   the first frame a person could look at.
5. **Spike five `AX` voice calls against one Melee sound bank** (phase 6, out
   of order deliberately). Audio is the only estimate in this plan with no
   measurement behind it, and it is cheaper to learn that now than after
   phase 5.

### The critical path

Phases 3 → 4 → 5 are strictly sequential: nothing draws until the SDK
boundary is closed, nothing navigates until it draws, and no match runs until
the scene flow does. Phases 6 and 7 are not on that path — audio and the
remaining fighters, stages and items can proceed in parallel once phase 5
lands, and they are where a second pair of hands would help most.
