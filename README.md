# PgrOS

A fast, quiet, chat-first operating layer for the **LilyGo T-LoRa Pager**, built on top of
[Meshtastic](https://github.com/meshtastic/firmware).

PgrOS replaces Meshtastic's on-device screen stack with an LVGL interface designed around
the two things the hardware is actually used for — **messaging and location** — and adds
persistent chat history, a WiFi portal, and a strict radio coexistence policy. The mesh
stack itself is untouched, so a PgrOS pager is a fully compatible Meshtastic node and works
with the official Meshtastic phone apps.

> **Status: pre-hardware.** The firmware builds, and the design and code are complete for
> the scope described below, but it has not yet been flashed to a physical pager. Anything
> that has not been verified on-device is called out honestly in
> [`docs/STATUS.md`](docs/STATUS.md). Do not read this README as a claim that it has been
> tested on hardware.

## What it does

* **Messaging that feels like a phone.** Threads, bubbles, relative timestamps, delivery
  state, and a composer built for the pager's physical keyboard.
* **History that survives reboots.** Chat is an append-only log on flash, not RAM. Every
  message stores a snapshot of who sent it, so channel history stays attributable even
  after a node ages out of the node database.
* **Built to boot fast.** The UI runs as its own task and never waits for the LoRa stack,
  so the panel is interactive while the radio is still calibrating. The design targets
  first pixel in ~120 ms and interactive well under half a second — those are targets
  derived from what work sits on each path, **not measurements**, since this has not run
  on hardware yet. See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md#boot-how-fast-is-achieved).
* **Silent unless you ask.** No boot chirp, no keypress beep. The amplifier is muted before
  any audio code can initialise.
* **Bluetooth and WiFi never run together.** A single state machine owns both radios.
  Switching between Bluetooth and WiFi restarts the device, because on ESP32 the Bluetooth
  controller's memory is released irreversibly and cannot be reclaimed without a reboot —
  PgrOS says so before it does it, rather than appearing to hang. Off ↔ WiFi client ↔
  hotspot are live transitions.
* **A WiFi portal.** Run the pager as an access point and get a browser-based chatroom and
  photo gallery your phone can upload to.
* **Bluetooth pairing you can actually complete.** The six-digit passkey is shown on screen
  in a modal, which the stock UI does not do well on this panel.

## Build

Requires [PlatformIO](https://platformio.org/) and a `bash` (Git for Windows is fine).

```bash
git clone --recurse-submodules https://github.com/Archaejohn/PgrOS.git
cd PgrOS
./scripts/build.ps1                      # or: pio run -d vendor/firmware -e pgros
./scripts/build.ps1 -Target upload -Port COM7
```

`vendor/firmware` is a pinned Meshtastic checkout. `scripts/apply-patches.sh` applies the
PgrOS patch set to it before compiling; `build.ps1` does this for you.

The T-LoRa Pager requires DFU mode to flash: hold **BOOT**, tap **RESET**, release **BOOT**.

## Relationship to upstream Meshtastic

PgrOS vendors Meshtastic as a git submodule pinned to a known-good commit, rather than
forking it. New features live in `src/pgros/`. Changes to Meshtastic itself live in
`patches/` and are kept deliberately small:

* **`patches/upstream/`** — genuine Meshtastic bugs found while building PgrOS. Each one
  ships with a root-cause writeup written to be filed as an issue and opened as a PR.
  These are meant to disappear as they land upstream.
* **`patches/integration/`** — build hooks that let PgrOS compile as an overlay. Not bugs,
  not upstreamable.

`git -C vendor/firmware diff` is therefore always the complete, current set of changes to
Meshtastic — there is no divergence to untangle later.

### Bugs found so far

| # | Summary | Writeup |
|---|---------|---------|
| 0001 | T-LoRa Pager keyboard emits random characters: the TCA8418 event FIFO is read as if it were an array, skipping events and decoding stale slots as keystrokes | [writeup](patches/upstream/0001-tlora-pager-keyboard/README.md) |
| 0002 | I²C keyboards are polled at a flat 300 ms and characters are emitted on key release, so typing lags by up to a third of a second per character | [writeup](patches/upstream/0002-keyboard-poll-latency/README.md) |
| 0003 | `BHI260APSensor.cpp` writes `screen->steps` unguarded, so any board with that IMU fails to build when the stock screen is excluded | [writeup](patches/upstream/0003-bhi260ap-screen-steps/README.md) |

## Documentation

| Document | What it covers |
|----------|----------------|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Boot staging, threading model, storage format, coexistence, stability |
| [`docs/STATUS.md`](docs/STATUS.md) | What is built, what is verified, what is not |
| [`docs/HARDWARE.md`](docs/HARDWARE.md) | T-LoRa Pager pinout and peripheral notes |

## Licence

GPL-3.0, matching Meshtastic.
