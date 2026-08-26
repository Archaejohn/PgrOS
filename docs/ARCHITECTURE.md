# PgrOS architecture

PgrOS is a UI and services layer for the LilyGo T-LoRa Pager that runs **on top of**
an unmodified Meshtastic firmware core. It replaces Meshtastic's on-device screen
stack with an LVGL interface designed for chat and navigation, and adds services
(persistent chat history, WiFi AP portal, radio coexistence) that the stock firmware
does not have.

It is explicitly **not** a fork. Meshtastic's mesh, routing, crypto, module and phone
API layers are used as-is so that PgrOS remains wire-compatible with the mesh and with
the official Meshtastic phone apps.

## Repository layout

```
PgrOS/
  vendor/firmware/        git submodule -> meshtastic/firmware, pinned
  src/pgros/              all PgrOS code (compiled into the vendored tree)
    core/                 boot, event bus, panic guard, policy
    hal/                  display, keyboard, haptics, audio mute, power
    store/                persistent append-only storage
    ui/                   LVGL shell, theme, widgets, navigation
    ui/apps/              the individual screens
    net/                  radio coexistence, WiFi, web portal
  patches/upstream/       Meshtastic bugs: writeup + patch, for issues/PRs
  patches/integration/    build hooks so PgrOS compiles as an overlay
  data/www/               web portal assets, packed into LittleFS
  pgros.ini               the [env:pgros] PlatformIO environment
  scripts/                build + patch tooling
```

### Why an overlay instead of a fork

The user requirement is that Meshtastic bugs we find are filed and PR'd upstream, while
PgrOS features stay ours. A fork makes that hard: after a few weeks of divergence,
extracting a clean upstream diff means archaeology. Here, `git -C vendor/firmware diff`
*is* the set of changes to Meshtastic, always, and it is kept small and deliberate.
`scripts/apply-patches.sh --check` shows exactly what we have changed.

The build root is `vendor/firmware` (so all of Meshtastic's relative paths keep working);
`pgros.ini` adds one environment that pulls `src/pgros` in via `build_src_filter`.
A single integration patch teaches `vendor/firmware/platformio.ini` to include it.

## Boot: how "fast" is achieved

Stock Meshtastic runs a long synchronous `setup()` and the screen only becomes useful
at the end. PgrOS decouples the two: **the UI is a separate FreeRTOS task that starts
early and never blocks on the mesh stack.**

| Stage | When | What runs | Target |
|-------|------|-----------|--------|
| 0 | first thing in `setup()` | Power rails, panel init, backlight still **off**, splash drawn direct to the panel via LovyanGFX | < 120 ms to first pixel |
| 1 | immediately after | Backlight ramps up on an already-drawn frame (never flash a white panel), UI task spawned on core 1, LVGL initialised against PSRAM buffers | < 350 ms to interactive |
| 2 | concurrent | Meshtastic `setup()` continues on the main task: FS, NodeDB, radio, GPS, modules | UI stays responsive throughout |
| 3 | as they land | Each subsystem reports readiness on the event bus; the UI fills in status bar icons progressively | — |

The user can be scrolling the message list while the LoRa radio is still calibrating.
Perceived boot time is stage 1, not stage 3.

Two rules keep this honest:

* **Nothing on the UI task may block.** No I2C, no SPI to the radio, no file writes
  larger than a few KB. The UI task talks to the rest of the system only through the
  event bus and pre-built snapshots.
* **The splash is drawn before the backlight comes on.** Backlight-then-draw is the
  single most common cause of a boot that *looks* slow even when it isn't.

## Threading model

```
  core 0                                  core 1
  ------                                  ------
  Meshtastic main loop                    pgros::UiTask
    radio, router, modules, PhoneAPI        LVGL tick + timer handler
    NodeDB, GPS, power                      renders from UiState snapshots
         |                                        ^
         |  EventBus::post()  (FreeRTOS queue)    |
         +----------------------------------------+
                     lock-free, one-way
```

* **LVGL is touched from the UI task only.** This is not negotiable — LVGL is not
  thread-safe, and cross-task LVGL calls are the classic source of the random heap
  corruption panics we are trying to avoid.
* Producers (mesh, GPS, power, WiFi) never touch UI objects. They `post()` a small
  POD event. The UI task drains the queue each frame and mutates its own state.
* Shared read-mostly data (node names, channel names) is copied into the event at post
  time rather than pointed at, so the UI never dereferences NodeDB while the mesh task
  is mutating it.

## Storage

Persistent chat is a hard requirement, so it does not live in RAM or in Meshtastic's
in-memory `MessageStore`.

`store/ChatStore` is an **append-only log per thread** on LittleFS:

```
/pgros/ch/<channel>.log     broadcast threads, one per channel index
/pgros/dm/<nodenum>.log     direct message threads
```

Each record carries a **length trailer as well as a length header**, which lets the
store walk the file *backwards* from EOF. Opening a conversation reads only the last N
records instead of scanning from the start — that is what keeps the messages app fast
regardless of history depth.

Each record also stores a **snapshot of the sender's short and long name** alongside the
node number. Node names are resolved at receive time and frozen into the log, so history
remains attributable even after the sender ages out of NodeDB. This directly satisfies
"channels must have who the sender is."

Crash safety: every record is CRC-checked and self-delimiting. A torn write at the tail
is detected on load and truncated, rather than corrupting the whole thread. Appends are
followed by an explicit flush; the store never rewrites earlier bytes.

Compaction is by whole records into a new file, then rename — never in place.

## Radio coexistence

The ESP32-S3 shares one 2.4 GHz radio between BLE and WiFi. Running both is a
well-known source of instability, and the requirement is explicit: never both.

`net::RadioCoex` is a small state machine and the **only** thing permitted to enable or
disable either stack:

```
        OFF
       /  |  \
     BT  STA  AP
```

Every transition goes **through OFF** with a real teardown and a settle delay — never a
direct BT to WiFi hop. Callers request a state; they do not call `WiFi.begin()` or the
NimBLE setup directly. Any code path that wants the other radio must be able to tolerate
being told "no, and here is why".

## Silence by default

The pager ships with a buzzer, an I2S amplifier and a DRV2605 haptic driver. The
requirement is that a fresh boot and ordinary use make **no sound** unless the user opts
in.

* The amplifier is explicitly driven low via the XL9555 expander (`EXPANDS_AMP_EN`)
  during stage 0, before any audio code can initialise — an unmuted amp powering up is
  what produces the boot pop.
* `core/Policy` owns notification preferences: sound, haptics and backlight behaviour,
  each independently switchable, all defaulting to off except a subtle haptic on
  keypress which is also off by default.
* PgrOS does not reuse Meshtastic's `buzzer_mode` for UI feedback, because that setting
  is also what the phone app writes; a user enabling external notifications on their
  phone should not start making the keyboard click.

## Stability

* **Panic capture.** A last-gasp handler writes the panic reason, program counter and
  a short backtrace to `/pgros/crash.log` before reset. On the next boot PgrOS surfaces
  it in Settings rather than silently rebooting, so failures are diagnosable from the
  device.
* **Watchdogs.** The UI task feeds a task watchdog; a hung render is recovered rather
  than wedging the device.
* **Bounded allocation.** LVGL draws from a dedicated PSRAM pool with a fixed ceiling,
  so a runaway UI cannot starve the mesh stack of heap.
* **No dynamic allocation on the receive path.** Incoming-message events are fixed-size
  PODs from a preallocated queue.

## Compatibility contract

PgrOS must not regress the phone app. Concretely, PgrOS does not modify or disable:

* `PhoneAPI` and its BLE/serial transports
* `Router`, `ReliableRouter`, `FloodingRouter`, crypto, or the protobuf definitions
* `NodeDB` persistence or the config/moduleConfig schema
* any `MeshModule` that ships enabled by default

PgrOS consumes these interfaces; it does not reach inside them. Where PgrOS needs
behaviour Meshtastic does not expose, the preferred order is: (1) use an existing
observer/callback, (2) add a PgrOS-side module, (3) only then patch the vendored tree —
and if we patch it, it gets a writeup in `patches/`.
