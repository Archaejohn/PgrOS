# Integration patch 0001 — PgrOS build and boot hooks

**Not a bug fix. Not upstreamable.** This patch exists only so PgrOS can compile as
an overlay on the vendored tree. It is kept separate from `patches/upstream/` so that
"changes we want Meshtastic to take" and "changes we need to build" never get confused.

**Applies to:** `meshtastic/firmware` @ `68bfe015e`
**Touches:** `platformio.ini` (3 lines), `src/main.cpp` (17 lines)

## What it does

### 1. `platformio.ini` — include the PgrOS environment

Adds `../../pgros.ini` to `extra_configs`. That file defines `[env:pgros]` and lives
outside the vendored tree, so the submodule stays a clean upstream checkout and the
PgrOS build configuration is versioned with PgrOS rather than with Meshtastic.

### 2. `src/main.cpp` — two call sites, both behind `#ifdef PGROS`

Everything is guarded by `PGROS`, which only `[env:pgros]` defines. Every other
environment in the tree compiles byte-identically to upstream.

| Hook | Location | Why there |
|------|----------|-----------|
| `pgros::earlyBoot()` | immediately after `initSPI()` | The earliest point at which the SPI bus exists, so it is the earliest point we can talk to the ST7796. Everything after it — `fsInit()`, `NodeDB`, `initLoRa()` — then runs behind a display that is already showing something. |
| `pgros::begin()` | after `inputBroker->Init()` | Needs `setupModules()` to have run (so `MeshBridge` can observe `textMessageModule`) and `inputBroker->Init()` to have run (so the keyboard and rotary encoder are registered before the UI subscribes). |

Plus one `#ifdef PGROS`-guarded `#include "core/Boot.h"`.

## Why hooks at all, rather than something less invasive

Three alternatives were considered and rejected:

* **Override the weak `lateInitVariant()`.** It is a genuine weak symbol and PgrOS
  could define it with no patch — but it fires at `main.cpp:~1179`, *after*
  `initLoRa()`. That is far too late for a splash screen; the whole point of stage 0
  is to have pixels on the panel while the slow initialisation happens.
* **Do everything from an `OSThread`'s first `runOnce()`.** Same problem: the first
  `loop()` iteration is after all of `setup()`.
* **Build with `HAS_TFT=1` and replace `tftSetup()`**, which is a genuine no-patch
  hook at `main.cpp:879`. Rejected for a different reason — see below.

Two hooks totalling 17 guarded lines was the smallest thing that actually delivers
fast boot.

## Note: why not `HAS_TFT`

Building as the MUI/device-ui variant would provide a patch-free UI hook, and it is
worth recording why PgrOS does not do that.

Under `HAS_TFT`, `NodeDB` defaults `config.display.displaymode` to `COLOR`
(`src/mesh/NodeDB.cpp:883`). `src/modules/Modules.cpp:125` then skips allocating
`InputBroker` entirely under `COLOR` — which also takes `SystemCommandsModule`,
`BuzzerFeedbackThread`, and every `ButtonThread` with it, since those are all
constructed inside `InputBroker::Init()`. On a device whose defining feature is a
physical keyboard, hand-rebuilding that stack is a much larger and more fragile
change than two `#ifdef`s.

The usual counter-argument is CPU clock: `main.cpp` calls `setCPUFast(false)` under
`#if !HAS_TFT`, which sounds like it would drop the pager to 80 MHz and starve LVGL.
It does not. `setCPUFast()` in `src/sleep.cpp:81` is guarded by
`!defined(T_LORA_PAGER)`, so on this board the call is a no-op and the CPU stays at
240 MHz either way. That was verified in the source before choosing this path.

## Maintenance

If the submodule pin moves and this patch stops applying, the fix is usually to
re-anchor the two `main.cpp` hunks — the anchors are `initSPI();` and the
`inputBroker->Init();` block, both of which have been stable for a long time.
Run `scripts/apply-patches.sh --refresh` after fixing up by hand.
