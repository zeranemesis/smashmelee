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
- [x] pin `doldecomp/melee` at `extern/melee` and compile its `objalloc.c`
      and `memory.c` in `melee_hsd_upstream_tests`, driven through the same
      behavioral assertions the port's own allocator answers; this is phase 0
      of [PLAN.md](PLAN.md) and the mechanism phase 2 uses for every
      subsequent unit;
- [x] run upstream's `class`, `object`, `list` and `id` beside the port's
      through the same assertions.  This found that `ref_DEC` released a
      reference one call early: HSD's counter holds the references beyond the
      first and reports the release when it was already zero, wrapping past it
      to `HSD_OBJ_NOREF`, where the port decremented first and reported on
      reaching zero.  It also established that `hsdSearchClassInfo` is a host
      addition — upstream's reads a hash nothing in the decompilation
      populates, so it always answers NULL;
- [x] give the upstream target a GX to talk to: `tests/hsd/gx_record.cpp`
      implements 84 entry points, each defined against Aurora's own
      declaration so a drifting signature does not compile.  Eighty-one
      append a line to a trace instead of drawing; the three `GXGetTexObj*`
      getters answer from the texture object, because HSD reads its
      dimensions back out of GX and branches on them.  Pointers are numbered by first appearance
      rather than printed as addresses, and the matrix entry points record the
      matrix rather than its address, so a recorded frame compares equal
      across runs and hosts.  That unblocked the whole render half —
      `jobj`, `dobj`, `mobj`, `pobj`, `tobj`, `cobj`, `lobj`, `tev`, `texp`,
      `texpdag`, `state`, `shadow`, `robj`, `wobj`, `displayfunc` — and the
      particle units that own the skinning helper.  Setting a camera current now records a viewport, a
      scissor and the console's own perspective matrix, and the suite asserts
      the shape of that frame as text and its computed terms with a tolerance;
- [x] write the first on-disc converter,
      `src/melee_port/upstream/archive_convert.cpp`, and feed its output to
      upstream's own loader: a DAT built in the console's layout —
      big-endian, 32-bit offsets, explicit relocation table — becomes host
      `HSD_Joint` structures, and `HSD_JObjLoadJoint` builds the game's joint
      tree out of them, with the matrices composing.  It reads through
      `Archive` rather than through a `<Name>32b` wire struct, because an HSD
      pointer field is not self-describing: a stored zero is the first byte of
      the data section or a null pointer depending on a relocation table in a
      different part of the file, and a wire struct cannot tell those apart;
- [x] convert the rest of a model — `HSD_DObjDesc`, `HSD_MObjDesc`,
      `HSD_Material`, `HSD_PObjDesc` and `HSD_VtxDescList` — and **draw it
      through the game's own display path**.  A DAT in the console's layout
      goes in, upstream's `jobj`, `dobj`, `mobj` and `pobj` walk it and talk
      to GX, and the recorder captures twenty-nine calls: a TEV stage, the
      pixel-engine state, a lighting channel, the position matrix, the vertex
      binding and the draw.  The sequence is asserted, which makes it the
      golden trace phase 3 is built on;
- [x] convert the texture chain — `HSD_TObjDesc`, `HSD_ImageDesc`,
      `HSD_TlutDesc`, `HSD_TexLODDesc` and `HSD_TObjTevDesc` — and draw a
      *textured* model through the same path.  The recorded frame grows the
      whole texture half: a texture matrix, a texture object built from the
      image's own dimensions and format, the LOD state, the bind, and a
      generated texture coordinate.  Pixels and palette entries stay in the
      archive, in the console's tiled layout, because Aurora's GX reads those
      formats natively;
- [x] settle the threading model, by measuring instead of choosing.  The plan
      framed it as coroutines versus preemptive threads; the decompilation
      answered that **Melee's shipping code creates no OS threads at all** —
      `OSThread` appears four times in 1034 files, once in the debug console
      and the rest in the Metrowerks debugger stub, and the SDK is not
      decompiled, so DVD and audio threading is Aurora's.  What the game
      actually uses is interrupt masking to guard against *handlers* (225
      calls), three alarm timers, and SDK completion callbacks.  So:
      `src/melee_port/upstream/os_scheduler.cpp`, one thread, a timebase that
      advances one exact NTSC field — 675675 ticks, no remainder — alarms that
      fire at their own instant, and a mask that really holds a handler off.
      Determinism is asserted, not hoped for;
- [x] fix the ID-table truncation.  Upstream keys its table on `(u32) joint`,
      the low word of a descriptor's address, and five places look one up that
      way — so on a 64-bit host two joints four gigabytes apart are one key,
      and a constraint or a skinning weight quietly follows the wrong bone.
      There is a test that demonstrates the collision directly.  The key
      cannot change without changing upstream, so the storage changed instead:
      joints come from one allocation whose size is a power of two and whose
      base is aligned to that size, which cannot straddle a boundary its own
      size, so every low word in it is distinct by construction;
- [x] convert what a primitive's union holds, in all three of its forms — a
      joint for a rigid primitive, an `HSD_ShapeSetDesc` for a morph target,
      and a table of `HSD_EnvelopeDesc` for a skinned one.  The envelope path
      leans on the relocation-table rule twice: both its terminators are
      pointer fields the table does not name, and a run ended by a *relocated*
      zero would be a run whose last entry weights the joint at offset zero.
      It also makes the joint conversion re-entrant, because a skinned
      primitive names joints and is reached from inside its own joint's
      conversion — a bone weighted from two runs still comes back as one
      object, which is what lets the skinning follow the animated matrix;
- [x] establish, rather than assume, that a recorded frame cannot be compared
      whole.  `HSD_TExpSetReg` in upstream's `texp.c` declares `GXColor
      reg[8]`, never initializes it, and writes only the components a constant
      names before handing the colour to GX — so `GXSetTevKColor`'s alpha and
      `GXSetTevColor`'s rgb are read before they are written, in the shipped
      game.  Compiling upstream with `-ftrivial-auto-var-init=pattern` turns
      both into `0xAA`, which is the proof.  Golden frames exclude them;
- [x] answer `GXSetArray`'s extent and byte order from what the load
      recorded, in `src/melee_port/upstream/gx_array_registry.cpp`.  Vertex
      data is not a structure, so it stays in the archive in the console's
      byte order, and Aurora's backend is told so rather than left to guess;
- [x] adapt `GXSetArray`.  Aurora's `TARGET_PC` form takes the array's byte
      length and byte order as well, because it writes a 64-bit base into the
      command stream and the backend copies the array out instead of reading
      it where it lies.  The prelude maps upstream's three arguments onto
      Aurora's five and asks the port for the other two, which is the honest
      shape of the question rather than a zero passed quietly;
- [x] compile and run fifteen upstream `sysdolphin` units in
      `melee_hsd_upstream_tests` — `objalloc`, `class`, `object`, `list`,
      `id`, `fobj`, `aobj`, `mtx`, `archive`, `util`, `random`, `quatlib`,
      `memory`, `hash`, `spline` — with Aurora's matrix and vector
      implementations linked in.  The 23 `MTX` helpers the plan expected to
      write were already there, macro-aliased onto Aurora's `C_MTX*`;
- [x] assert that upstream's `HSD_ArchiveParse` refuses a big-endian
      container on a little-endian host, which is the converters'
      justification stated in upstream's own code;
- [x] drive `quatlib`'s interpolator through all three of its branches.  Its
      third is the one the Windows job kept failing to compile: `M_PI_2` is
      not in standard C, and neither is `M_PI` in `mtx.c` — see
      [UPSTREAM_NATIVE_SPIKE.md](UPSTREAM_NATIVE_SPIKE.md).  That branch,
      between opposed quaternions, writes a perpendicular into `out` and then
      interpolates against the `q` it was handed rather than the
      perpendicular, so the write only survives when the caller passes one
      quaternion as both — and `lb_00B0.c` passes three.  On the path the game
      takes, the weights cancel and the result is the zero quaternion.  It is
      asserted as such: a reimplementation that corrected it would diverge
      from the disc;
- [x] convert the scene around the model: `HSD_CObjDesc`, `HSD_WObjDesc`,
      `HSD_LightDesc` and `HSD_FogDesc`.  The camera's assertion is
      differential rather than field-by-field -- the same camera built by hand
      and converted from bytes produce byte-identical GX frames, so any field
      read from the wrong offset moves a number in the trace.  Each of the
      four carried something a layout table would not have said:
      `HSD_CObjDesc` is a union of three camera shapes over a shared head with
      `projection_type` at +0x06 choosing between two floats at +0x30 and
      four; the up vector is a pointer to a `Vec3` that `CObjLoad` reads only
      when bit 0 of the flags is set; `HSD_LightDesc`'s union arm depends on
      *two* fields, the type in `flags & LOBJ_TYPE_MASK` and then `attnflags`
      choosing raw attenuation over distance attenuation, with an ambient or
      infinite light never reading the union at all; and `HSD_FogAdjDesc`
      carries a `Mtx44` where the rest of HSD uses a 3x4 `Mtx`.
      `Archive::scene_model_joints()` closes the model half of a scene -- a
      public symbol to a list of joint offsets, on the layout validated
      against GALE01's `MnMaAll.dat`.  The camera, light and fog lists in the
      same scene root get no accessor on purpose: their array shape has not
      been checked against a real archive, and that is now the shortest thing
      the disc is needed for;
- [x] convert the last two structure kinds a joint's union can hold,
      `HSD_Spline` and the particle `HSD_SList`, so a whole archive converts.
      Both had a trap a wire struct would have walked into.  `numcv` is not
      the control-point count -- it divides parameter space, and the count it
      implies is `numcv` for a linear spline, `3*numcv - 2` for a Bezier and
      `numcv + 2` for a B-spline or cardinal one, each read off
      `splGetSplinePoint`'s own indexing in both of its branches; converting
      only `numcv` of them would leave upstream reading past the host array on
      a curve the console draws.  And the particle list's `void* data` is not
      a pointer: `HSD_JObjLoadJoint` does `*(u32*) &slist->data |= 0x80000000`
      and `HSD_JObjDisp` reads a six-bit bank and a 24-bit offset out of it,
      so it is a packed integer the relocation table does not name -- read
      with `data_word`, because asking `Archive` for a pointer correctly
      refuses an unrelocated non-zero word.  A converted linear spline now
      evaluates through upstream's own `splGetSplinePoint` and
      `splArcLengthPoint` to the points it was built from;
- [x] close the SDK boundary's last gap, the `OS` arena and heap, and boot
      the game with it.  The measurement reversed the plan's assumption:
      Aurora already ships a complete SDK allocator
      (`extern/aurora/lib/dolphin/os/OSAlloc.cpp`, already on the runtime's
      link line), so what was missing was a heap the *conformance target*
      could link — that target takes no Aurora library, because
      `aurora::core` drags in SDL, fmt, abseil and sqlite.
      `src/melee_port/upstream/os_arena.cpp` is written to the same observable
      contract — 32-byte cells, first fit, address-ordered coalescing free
      list, an `OSCheckHeap` that reports real free space because
      `objalloc.c` branches on it — over one host block sized to a power of
      two and aligned to itself, so the low word of *every* HSD allocation is
      distinct, not just a joint descriptor's.  With it in place
      `tests/hsd/upstream_boot.cpp` runs `gmmain.c`'s own bring-up unchanged:
      four `HSD_SetInitParameter` calls, `HSD_AllocateXFB(2, &GXNtsc480IntDf)`,
      `HSD_GXSetFifoObj(GXInit(HSD_AllocateFifo(0x40000), 0x40000))`,
      `HSD_InitComponent()`.  Out of 24 MiB of arena come two framebuffers
      614 400 bytes apart, a 256 KiB graphics fifo, a 512 KiB audio heap, a
      22 MiB main heap, a spent arena, and a 26-call GX transcript of the
      console's opening frame — held as a golden trace.  **Thirty-eight
      upstream units** now compile and run, `fog`, `video` and `initialize`
      among them;
- [x] fix the golden traces the boot exposed as order-dependent.
      `HSD_GXInit` ends with `HSD_StateInvalidate(-1)`; before a boot existed
      nothing invalidated `state.c`'s cache, its statics sat at zero, four
      material fields happened to want zero, and the recorded frames silently
      omitted `GXSetAlphaUpdate`, `GXSetDstAlpha`, `GXSetDither` and
      `GXSetCullMode` — with the second drawing case's trace shorter than the
      first's *because the first had run*.  `gx::reset()` now invalidates
      before it clears, the pairing `HSD_GXInit` itself uses, and both traces
      are the console's;
- [x] carry the SDK spellings Aurora's Dolphin headers omit in
      `include/melee/port/dolphin_compat.h`, and shadow upstream's
      `Runtime/platform.h` from `cmake/MeleeUpstream.cmake` so its `ssize_t`
      declaration stops colliding with the host libc;
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
