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
- A hand-written HSD subset decodes and renders original main-menu geometry
  through Aurora GX, with joint animation at a fixed 60 Hz.
- An offline suite (`tests/hsd`, 61 cases) pins that subset's behavior and
  runs in CI on GCC with sanitizers, Clang, and MSVC.
- The `bootstrap.cpp` menu flow is hand-written, not the game's own.

---

## Phase 0 — Make upstream source available to the build

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

**Done when** `cmake --build` compiles one upstream translation unit
(`objalloc.c`) and the suite runs its cases against both implementations.

**Size.** Small. One submodule, one header, one CMake target.

## Phase 1 — Close the Dolphin SDK boundary

**Objective.** Every symbol upstream references either exists or is
deliberately stubbed, with the stub's consequence written down.

Work, in descending order of leverage:

| Group | Count | Nature |
|---|---|---|
| `MTX` projection and rotation helpers | 23 | Textbook matrices; write and unit-test them |
| `OS` threads, alarms, interrupts, contexts | 21 | The real work of this phase — see below |
| `VI` retrace callbacks and frame buffers | 7 | Bridge onto Aurora's swapchain and the 60 Hz clock |
| `GX` (`GXSetMisc`, `GXWaitDrawDone`, `GXInitFogAdjTable`, `GXSetCopyClamp`, `GXSetTevClampMode`, `GXNtsc480IntDf`) | 6 | Aurora extensions or no-ops |
| `DC`/`IC` cache operations | 3 | No-ops on a coherent host |
| `PADSetSamplingRate`, `CARDFormatAsync`, `VIPadFrameBufferWidth`, PAD constants, `OSContext` fields | ~13 | Header-level gaps |

`GXSetArray` takes five arguments in Aurora where the console SDK takes
three. The two extra are a size bound and a little-endian flag — Aurora's own
answer to vertex byte order — so upstream's two call sites in `pobj.c` are
adapted rather than worked around.

**On `OS` threads.** Melee runs DVD and audio work on OS threads with alarms
and interrupt masking. This repository already vendors `libco`; cooperative
coroutines driven from the fixed 60 Hz step reproduce the console's
scheduling far more faithfully than preemptive host threads, and keep the
simulation deterministic. Preemption is the wrong default here.

**Done when** upstream's `sysdolphin/baselib` links with no unresolved
symbol, and each stub is listed in a table with what it does not do.

**Size.** Medium. A few hundred lines of shims, plus the threading model,
which is the one genuinely design-sensitive piece.

## Phase 2 — Swap the hand-written HSD for upstream's

**Objective.** `src/melee_port/hsd/` is gone, and upstream's
`sysdolphin/baselib` is doing the work, with the offline suite unchanged.

1. Write `HSD_*32b` wire twins and `byteswap_hsd_*()` converters for the ~35
   on-disc structures, following `src/port/byteswap.cpp`. The existing
   decoders in `scene.cpp` and `animation.cpp` are the specification: every
   field offset they read is already correct and already covered by tests.
2. Replace unit by unit, keeping the suite green at each step:
   `objalloc` → `class`/`object` → `list`/`id` → `mtx` → `fobj`/`aobj` →
   `jobj` → `dobj`/`mobj`/`tobj`/`pobj` → `cobj`/`lobj`/`fog` → `robj` →
   `tev`/`texp` → `shadow`.
3. Retire each host decoder only once its upstream replacement passes the
   cases that pinned it.

The suite already pins `objalloc`, `class`/`object`, `list`, `id`, `fobj`
(twelve stream shapes) and `aobj` (seven playback modes) against upstream's
own behavior, so most of this phase has its acceptance test written already.

**Done when** no file remains under `src/melee_port/hsd/`, and all 61 cases
plus whatever the swap adds are green on three toolchains.

**Size.** Large but bounded: ~35 converters, ~76 units to bring up.

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

**Size.** Large. ~107,000 lines, but mechanical: it compiles today.

## Phase 5 — A match

**Objective.** Two fighters, one stage, a complete versus match.

`melee/pl` (players, 6k) → `melee/cm` (camera, 5k) → `melee/if` (HUD, 10k) →
`melee/ft` common (the shared fighter engine) → one fighter → `melee/gr` one
stage → `melee/it` minimal (items off).

**Done when** a two-player match starts from the character select, plays to a
result screen, and replays identically from the same inputs.

**Size.** Large. `ft/kinds/ftCommon` is the shared engine; a single fighter
on top of it is comparatively small.

## Phase 6 — Sound

**Objective.** Music and effects.

musyx is already a submodule. Melee's path runs `melee/sfx` over AX and the
ARAM queue; the 36 unresolved audio symbols from the link measurement belong
here.

**Done when** menu music, stage music, and hit effects play in sync with the
simulation.

**Size.** Medium to large, and the least de-risked part of this plan — no
measurement in this session touched it.

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
fighters and stages are largely independent once the engine is up.

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
will.

**Assets stay out.** No ISO, no `main.dol`, no game data in this repository,
ever. A legally obtained disc image is a runtime input.

## Risks, in order of how much they could cost

1. **Aurora's GX does not draw what the console drew.** TEV chains,
   fog, lighting, and shadows are where this shows. Mitigation: golden frames
   from phase 3 onward, so divergence is caught on the first scene rather
   than during fighter bring-up.
2. **Audio.** Nothing has been measured. Mitigation: spike musyx against one
   Melee sound bank before committing phase 6's shape.
3. **The threading model.** A wrong choice in phase 1 is expensive to undo
   because everything above it inherits the scheduling. Mitigation: decide it
   deliberately, with determinism as the acceptance criterion.
4. **The 33 structure-layout assertions.** They exist to verify the match
   against the original binary and are meaningless on a 64-bit host.
   Mitigation: disable for host builds, and record that the `*32b` twins are
   what now guarantees the layout.
5. **Upstream drift.** The submodule is pinned; updating it is a deliberate
   act with the suite as the gate.

## The next five actions

1. Pin `doldecomp/melee` as `extern/melee` and add the `melee_upstream` CMake
   target (phase 0).
2. Promote the spike's shim to `include/melee/port/dolphin_compat.h` and
   compile upstream's `objalloc.c` in the test build (phase 0).
3. Write the 23 `MTX` helpers with unit tests (phase 1) — self-contained, and
   it unblocks `cobj`.
4. Decide the threading model and write it down (phase 1).
5. Write `HSD_Joint32b` and `byteswap_hsd_joint()`, and check it against
   `HostScene`'s existing joint decode (phase 2) — the first converter, with
   its oracle already in the repository.
