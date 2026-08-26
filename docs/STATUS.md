# Status

Last updated: 2026-08-26, against `meshtastic/firmware` pin `68bfe015e`.

This document exists so nobody has to guess how far along this is. **PgrOS has never
been flashed to a physical T-LoRa Pager.** Everything below is either "compiles" or
"reasoned from the source" — nothing is "observed working on hardware", because no
hardware has been available.

## Build

```
Environment    Status    Duration
pgros          SUCCESS   00:03:58
RAM:   45.6%  (149,368 / 327,680 bytes internal)
Flash: 40.2%  (2,634,727 / 6,553,600 bytes, app partition)
```

The whole firmware links, including every app, LVGL 9.3.0, the web portal, and the
Meshtastic stack. `scripts/apply-patches.sh --reset && scripts/apply-patches.sh`
round-trips: the patch set reproduces the working tree byte for byte from a pristine
submodule checkout.

Headroom is comfortable. 45.6% internal RAM at link time is before LVGL's PSRAM draw
buffers are allocated (those do not count against this figure), and before any
runtime allocation.

## What is implemented

| Area | State |
|---|---|
| Boot staging (splash before backlight, UI task on core 1) | Written, compiles |
| LVGL 9.3 + ST7796 via LovyanGFX, reentrant `spiLock` around flushes | Written, compiles |
| Persistent append-only chat store, backward-walking reads | Written, compiles |
| Sender identity snapshotted into every stored message | Written, compiles |
| Messages / Conversation / Contacts apps | Written, compiles |
| Home launcher, Settings, Network, GPS, Diagnostics apps | Written, compiles |
| Status bar (battery, GPS, radio, signal, unread, clock) | Written, compiles |
| BLE pairing passkey modal | Written, compiles |
| Radio coexistence state machine (reboot-based BT↔WiFi) | Written, compiles |
| WiFi scan / join / saved networks / AP mode | Written, compiles |
| Web portal: chatroom + photo gallery, client-side downscale | Written, compiles |
| Silence policy (amp mute, haptics, buzzer), silent by default | Written, compiles |
| Crash / boot-loop capture | Written, compiles |
| Keyboard + rotary input bridge | Written, compiles |

## What is NOT done

* **Nothing has been run.** No boot, no render, no message sent or received, no
  pairing, no WiFi association, no upload. The first flash will find bugs.
* **Boot timing is a target, not a measurement.** The architecture doc claims first
  pixel in ~120 ms and interactive under ~350 ms. Those numbers come from the design
  (what work is on each path), not from a stopwatch. They need verifying.
* **WiFi mesh and voice calling are not built.** You scoped voice out at the start.
  The WiFi mesh (pagers extending each other's network with a shared chatroom and
  gallery) is also not implemented — the current portal is a single-device AP. This
  is the largest remaining feature.
* **The gallery has no thumbnailing.** Full images are sent to the browser and scaled
  in CSS. On a large gallery that will be slow over the pager's AP.
* **Read receipts** have a settings toggle but no wire implementation.
* **`Policy.alertsWhileCharging`** is stored and shown but not enforced; the charging
  state arrives on the event bus and `PolicyStore` does not track it. Marked with a
  comment in `Silence.cpp` rather than silently ignored.
* **`storeGpsTrack`** is a stored preference with no recorder behind it yet.
* **No automated tests.** There is no host-side test target for the chat store, which
  is the one component that would genuinely benefit from one — its record framing and
  recovery path are pure logic and could be tested off-device.

## Known risks for the first hardware bring-up

Ranked by how likely they are to bite, based on what the code actually does:

1. **SPI contention between the panel and the LoRa radio.** They share SPI2 with the
   SD card. `Display::flushCb` holds a reentrant wrapper around Meshtastic's
   `spiLock` for the transfer only. The reentrancy is required because
   `concurrency::Lock` self-deadlocks on re-entry and LovyanGFX nests transactions.
   If this is wrong, the symptom is either a hung UI task or intermittent radio
   corruption. Watch `display.lastFlushUs()` in Diagnostics.
2. **LVGL draw buffer allocation.** `heap_caps_aligned_alloc(32, …, MALLOC_CAP_SPIRAM)`
   is used because plain `ps_malloc` is documented (by device-ui's own source
   comments) to crash on this DMA path. There is a fallback to a smaller internal-RAM
   buffer, but the fallback path is untested.
3. **Panel geometry.** Native 222×480 presented as 480×222 via `offset_rotation = 3`,
   `offset_x = 49`. Taken verbatim from two independent proven sources, but an
   off-by-49 stripe is the classic failure and would be obvious immediately.
4. **The BLE passkey is generated once per boot.** That is upstream behaviour
   (`m_regenOnConnect` is false and Meshtastic never calls
   `regenPassKeyOnConnect()`), not something PgrOS introduced — but it means the
   modal shows the same code all session. Worth a follow-up.
5. **First-boot filesystem state.** `ChatStore::begin()` creates directories lazily
   and defers per-thread validation. The recovery path for a torn record has been
   reasoned through carefully but never exercised against a real interrupted write.

## Upstream bugs found

Three, all with root-cause writeups ready to file. See [`../README.md`](../README.md#bugs-found-so-far).

| # | Summary | Verified how |
|---|---|---|
| 0001 | TCA8418 key FIFO read as an array → random characters | Root-caused from the datasheet semantics and the code; **not** yet confirmed by reproducing on hardware |
| 0002 | I²C keyboards polled at a flat 300 ms → typing lag | Reasoned from code; latency improvement not measured |
| 0003 | `BHI260APSensor.cpp` breaks the build when the screen is excluded | **Confirmed** — this one actually broke our build, and the fix unbroke it |

Only 0003 is empirically confirmed. 0001 is a strong, specific root cause that matches
the reported symptom precisely, but honesty demands noting that "matches the symptom"
is not the same as "reproduced and fixed on the bench".

## Next steps, in order

1. Flash it. Confirm the panel comes up and the splash renders right-way-round.
2. Watch the boot log for `PgrOS: panel up`, `LVGL up`, `UI task running`, `ready`.
3. Type into a conversation and confirm the keyboard fix holds at speed.
4. Confirm the phone app still connects over BLE and sees messages both ways — that
   is the compatibility contract, and it is the thing most worth not breaking.
5. Measure real boot time and put the actual number in `ARCHITECTURE.md`, replacing
   the target.
6. File the three upstream issues and open PRs.
