# Melee native-port plan

## Target

- Game: Super Smash Bros. Melee
- Disc: `GALE01`, disc 0, revision 2 (USA v1.02)
- Upstream executable SHA-1: `08e0bf20134dfcb260699671004527b2d6bb1a45`
- Host foundation: Party Board plus Aurora

## Boot path observed in upstream

The reference entry point is `src/melee/gm/gmmain.c`. Its initialization order is:

1. `OSInit`, `VIInit`, `DVDInit`, `PADInit`, `CARDInit`, `OSInitAlarm`
2. debug level and arena setup
3. HSD framebuffer and GX FIFO allocation
4. `GXInit` and `HSD_InitComponent`
5. audio, memory, DVD, ARQ, snapshot, and movie subsystems
6. `gmMainLib_8015FBA4`
7. `gm_801A4510`, which enters the game scene flow

Aurora already provides host implementations for much of the Dolphin SDK boundary (OS, VI, DVD, PAD, CARD, GX, and matrix primitives). Melee's `sysdolphin/baselib` is the main missing engine layer between those APIs and the game code.

## Plan

[PLAN.md](PLAN.md) sequences the work from today's state to a running game,
with acceptance criteria and sizing for each phase. The milestones below
record what has been established; the plan says what happens next and in what
order.

## Milestones

### M0 — native shell (implemented)

- select and inspect a GALE01 rev 2 image;
- mount it through Aurora;
- initialize the native renderer/input/UI;
- enter a Melee-specific bootstrap loop;
- never call Mario Party 4 gameplay code.

### M1 — HSD core

- [x] add a host-width-safe implementation of the HSD object allocator;
- [x] verify alignment, allocation limits, statistics, and free-list reuse;
- [x] port the HSD root class/object model, inheritance lookup, lifetime counters,
      and reference counters;
- [x] port a deterministic GObj process scheduler with priority ordering and
      safe deferred removal;
- [x] port the HSD singly-linked collection allocator and the 101-bucket ID
      table used by scene/object dependency registration;
- [x] port the HSD host vector and matrix allocation pools used by camera,
      joint, and animation runtime objects;
- [x] port FObj allocation, descriptor materialization, lifecycle, and request
      state so host animation objects have the upstream-compatible foundation;
- [x] interpret bounded host-materialized FObj bytecode (constant, linear,
      spline, key channels; U8/S8/U16/S16/F32 fractions) and dispatch it to
      native object-update callbacks;
- [x] port AObj allocation, lifetime, playback state, and FObj ownership while
      explicitly rejecting unresolved GameCube object references;
- [x] materialize HSD `AnimJoint`/`AObjDesc`/`FObjDesc` trees into host-owned
      animation data, validating relocations, hierarchy cycles, and bytecode
      bounds before they can be attached to rendered joints;
- [x] execute materialized joint animations against explicit `HostJoint`
      mappings for rotation, translation, scale, and node visibility;
- [x] validate the upstream main-menu pair from `MnMaAll.dat` on GALE01 v1.02:
      `MenMainBack_Top_animjoint` and `MenMainBack_Top_joint` both contain 102
      corresponding hierarchy nodes;
- [x] decode and submit the original `MenMainBack` model through Aurora (86
      draw objects, 324 triangles) while advancing its joint animation at 60
      Hz, retaining `standScene` as the safe fallback;
- [x] resolve and apply the separately exported main-menu camera
      `ScMenMain_cam_int1_camera` from `MnMaAll.dat`;
- [x] decode and layer the animated `MenMainPanel_Top` hierarchy (106 joints,
      47 draw objects), including direct GX CLR0 formats expanded to RGBA8;
- [x] mirror `lb_80011E24` pre-order joint addressing for host animation
      subtrees and render `MenMainConTop_Top` (42 joints, 20 draw objects),
      the original layer containing the five top-level menu choices;
- [x] read Aurora PAD/keyboard state at 60 Hz and drive the top-level hover
      animation ranges from `mn_803EB3FC` with D-pad or analog-stick input;
- [x] provide non-persistent fallback keyboard controls when port 0 has neither
      a connected controller nor saved keyboard bindings (arrows and Z/X for
      menu navigation; WASD and the remaining GameCube controls for later
      gameplay bring-up), without overriding user configuration;
- [x] instantiate the five original `MenMainCursor_Top` hierarchies, attach
      them to the animated option joints, and reproduce the joint selection
      poses and visibility rules from `mn_8022B3A0`;
- [x] reproduce the native `MENU_KIND_VS` transition from `mn_8022DB10`,
      including the original VS hover/cursor animation ranges, five choices,
      and the B-button return path from `mn_8022D594`;
- [x] keep HSD AObj/FObj allocators process-global while multiple animation
      players are alive; this is covered by a two-player materialization
      regression and a 90-second MSVC AddressSanitizer menu smoke test;
- [x] establish an HSD logical NTSC render mode and GX frame-state bridge over
      Aurora's host swapchain;
- [x] initialize Aurora's host GX FIFO explicitly from the incremental Melee
      bootstrap; unlike the original `HuSysInit` path, this bootstrap does not
      otherwise reach `GXInit`;
- [x] advance the host HSD/GObj simulation on a bounded fixed 60 Hz clock,
      independent of Aurora's presentation cadence;
- [x] mount a local GALE01 v1.02 ISO/RVZ, expose its GameCube FST, and materialize the `standScene` model/joint transforms from `GmRgStnd.dat` without unsafe host-pointer casts;
- [x] preserve and validate HSD relocation records, including valid relocated
      zero offsets, instead of treating GameCube pointers as host pointers;
- [x] decode indexed position streams and render the supported `standScene`
      mesh hierarchy through Aurora GX;
- [x] resolve `HostScene::model_roots()` as host joint indices in the renderer
      instead of incorrectly treating them as archive offsets; a Windows
      capture now verifies original `MnMaAll.dat` geometry reaching the
      framebuffer (158 draw calls in the tested main-menu frame);
- [x] materialize the static HSD CObj camera (eye, interest, up vector,
      viewport, projection and depth planes), retaining a debug fallback for
      malformed or unsupported archives;
- [x] apply static HSD CObj viewport and scissor state through Aurora GX,
      including the render-mode scaling and field-jitter path used by
      `HSD_CObjSetCurrent`;
- [x] materialize host-safe `HSD_Material` records and apply their diffuse
      color and alpha to supported untextured draw objects;
- [x] materialize and render first-level HSD TObj images, including direct and
      palette-backed GX formats, with indexed TEX0 coordinates; complete TEV
      chains remain unsupported;
- [x] resolve the VS character-select model, camera, and `ANIM[3]` animation
      roots from `MnSelectChrDataTable` in `MnSlChr.usd` (173 model/animation
      joints, 201 draw objects and 4303 decoded triangles on GALE01 v1.02);
- [x] distinguish a relocated pointer to data offset 0 from a NULL field.
      HSD stores NULL as a zero word that the loader leaves out of the
      relocation table, so on a host that keeps offsets instead of addresses
      the two collide: every structure at the start of a data section was
      being read as absent. `Archive::kNullOffset` now carries NULL, and the
      head `HSD_PObjDesc` reads its vertex-descriptor and display-list fields
      through `data_pointer` like the rest of the chain, instead of accepting
      a raw console address as an offset;
- [x] read an animation opcode and its key count from the one byte HSD packs
      them into, matching `parseOpCode`/`parsePackInfo`; the interpreter was
      consuming two, so every real FObj stream decoded a byte out of step.
      The host interpreter now agrees with upstream's on every value across
      twelve stream shapes, regenerable with
      `tools/upstream_native_spike.py --fobj-reference`;
- [x] decode the head of an `HSD_PObjDesc` chain through the same routine as
      the entries behind it; two independent copies of that decoder are how
      the head came to read its pointer fields differently in the first place;
- [x] initialize the HSD object pools explicitly in `HSD_ObjInit` order
      instead of as a side effect of boot-time self-tests, and move that
      verification into the offline suite (see below);
- import the upstream `Runtime`, `sysdolphin`, and required `melee/lb` headers/sources;
- make pointer-width and endian assumptions explicit;
- compile object/class allocation, GObj scheduling, VI, and GX initialization;
- replace PowerPC-only assembly and cache operations at the platform boundary.

Exit criterion: `HSD_InitComponent` completes on x86-64 without a crash.

### Verification

The HSD core compiles against headers only — no Aurora library, no GPU, no
disc image — so its behavior is pinned down by an offline suite that builds
and runs in about a second:

```sh
cmake -S tests/hsd -B build/hsd-tests
cmake --build build/hsd-tests
ctest --test-dir build/hsd-tests --output-on-failure
```

`-DMELEE_TESTS_SANITIZE=ON` adds the address and undefined-behavior
sanitizers. The main build also produces the target unless
`-DMELEE_BUILD_TESTS=OFF` is passed. CI runs the suite on GCC (sanitized),
Clang, and MSVC for every push and pull request.

The suite builds synthetic HSD DAT containers in the on-disc GameCube layout
(`tests/hsd/dat_builder.*` and `tests/hsd/hsd_fixtures.*`) rather than
shipping any Nintendo asset, so a decoder change that drifts from the format
fails before it ever reaches a disc. Add a case there for every new structure
the port learns to read.

Reference behavior comes from `doldecomp/melee`; check a symbol against
upstream before "fixing" a difference, because HSD has several APIs whose
names do not describe what they do (`HSD_SListAppendList` splices after the
head rather than walking to the tail, for instance).

### M2 — first scene

- port the minimum `gm`, `sc`, `mn`, `lb`, and `db` dependency closure;
- load files directly from the mounted disc;
- enter `gm_801A4510` and display the first boot/menu scene.

### M3 — versus slice

- character select;
- one stage and two fighters;
- controller input, collision, camera, HUD, and match completion;
- deterministic 60 Hz simulation.

### Direction

Reimplementing Melee by hand does not reach a complete game: it is roughly
490,000 lines of C. `doldecomp/melee` is decompiled essentially in full, and
95.5% of it already passes a native x86-64 syntax check, so compiling those
sources against a host Dolphin SDK is the path M1's exit criteria describe.
What that measurement does and does not establish — and the pointer-width
fork it exposes — is written up in
[UPSTREAM_NATIVE_SPIKE.md](UPSTREAM_NATIVE_SPIKE.md), reproducible through
`tools/upstream_native_spike.py`.

### M4 — full-game coverage

- remaining fighters, stages, items, single-player modes, menus, save data, movies, and audio;
- regression tests against the original behavior;
- platform packaging.

## Porting rules

- Do not copy or commit game assets, an ISO, or `main.dol`.
- Keep upstream symbol names until behavior is understood.
- Prefer adapting Melee code to Aurora's Dolphin API over inventing a second compatibility layer.
- Treat every 32-bit pointer cast and big-endian data read as a review point.
- Keep the bootstrap build usable while M1/M2 are incomplete.
