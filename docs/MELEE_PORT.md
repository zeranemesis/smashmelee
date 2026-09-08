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

## Milestones

### M0 — native shell (implemented)

- select and inspect a GALE01 rev 2 image;
- mount it through Aurora;
- initialize the native renderer/input/UI;
- enter a Melee-specific bootstrap loop;
- never call Mario Party 4 gameplay code.

### M1 — HSD core

- [x] add a host-width-safe implementation of the HSD object allocator;
- [x] verify alignment, allocation limits, statistics, and free-list reuse at boot;
- [x] port the HSD root class/object model, inheritance lookup, lifetime counters,
      and reference counters;
- [x] port a deterministic GObj process scheduler with priority ordering and
      safe deferred removal;
- [x] establish an HSD logical NTSC render mode and GX frame-state bridge over
      Aurora's host swapchain;
- [x] mount a local GALE01 v1.02 ISO/RVZ, expose its GameCube FST, and decode the `standScene` roots from `GmRgStnd.dat` without unsafe host-pointer casts;
- import the upstream `Runtime`, `sysdolphin`, and required `melee/lb` headers/sources;
- make pointer-width and endian assumptions explicit;
- compile object/class allocation, GObj scheduling, VI, and GX initialization;
- replace PowerPC-only assembly and cache operations at the platform boundary.

Exit criterion: `HSD_InitComponent` completes on x86-64 without a crash.

### M2 — first scene

- port the minimum `gm`, `sc`, `mn`, `lb`, and `db` dependency closure;
- load files directly from the mounted disc;
- enter `gm_801A4510` and display the first boot/menu scene.

### M3 — versus slice

- character select;
- one stage and two fighters;
- controller input, collision, camera, HUD, and match completion;
- deterministic 60 Hz simulation.

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
