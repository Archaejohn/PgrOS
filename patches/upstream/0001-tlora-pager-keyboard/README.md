# Upstream bug report 0001 — T-LoRa Pager physical keyboard emits random characters

**Affected file:** `src/input/TLoraPagerKeyboard.cpp`
**Affected hardware:** LilyGo T-LoRa Pager (`T_LORA_PAGER`), TCA8418 I2C keyboard controller
**Found against:** `meshtastic/firmware` @ `68bfe015e6ab9ec2ab8f1657066898b7880eaf63` (develop, 2026-08-20), version 2.8.0
**Status:** root-caused; fix written and applied in PgrOS as `0001-tlora-pager-keyboard.patch`
**Severity:** high — the physical keyboard is the pager's primary input device

## Symptom

When typing at anything above a slow, deliberate pace, the pager inserts characters that
were never pressed. The spurious characters are frequently ones typed *earlier* in the
session, and keystrokes that were pressed sometimes go missing entirely. Typing slowly,
one key at a time with a clear pause between keys, mostly hides the problem.

## Root cause 1 — the key-event FIFO is read incorrectly (this is the random characters)

`TLoraPagerKeyboard::trigger()` overrides the (correct) base-class implementation and
reads the TCA8418 event FIFO like this:

```c
uint8_t count = keyCount();
if (count == 0)
    return;
for (uint8_t i = 0; i < count; ++i) {
    uint8_t k = readRegister(TCA8418_REG_KEY_EVENT_A + i);   // <-- BUG
    ...
}
```

On the TCA8418, `KEY_EVENT_A` (register `0x04`) is the **head of a 10-byte event FIFO**,
and **reading register `0x04` pops the FIFO** — every remaining event shifts down by one
slot. `KEY_EVENT_B..J` (`0x05`..`0x0D`) are the tail slots of that same FIFO; they are not
independent per-event registers, and they are only coherent as a single instantaneous
snapshot.

So for `count = 4` queued events the loop actually reads:

| iteration | register read | FIFO state before read | event actually returned |
|-----------|---------------|------------------------|--------------------------|
| `i = 0`   | `0x04`        | `[e1 e2 e3 e4]`        | `e1` — correct, pops     |
| `i = 1`   | `0x05`        | `[e2 e3 e4]`           | `e3` — **`e2` skipped**  |
| `i = 2`   | `0x06`        | `[e2 e3 e4]`           | `e4`                     |
| `i = 3`   | `0x07`        | `[e2 e3 e4]`           | **stale slot — garbage** |

Two things go wrong at once:

1. **Events are skipped.** Only iteration 0 pops. Later iterations read progressively
   further into the *unpopped* tail, so roughly every other real event is dropped —
   including *release* events, which desynchronises the `Idle`/`Held` state machine and
   leaves `last_key` stale.
2. **Reads run off the end of the valid FIFO into stale slots.** Those slots still hold
   key codes from *previous* keystrokes. They pass the `row`/`col` range check, get decoded
   through `TLoraPagerTapMap`, and are queued as real characters. **This is the source of
   the "random" characters, and it explains why they are so often characters the user
   genuinely typed a moment earlier.**

The bug only manifests when more than one event is queued between polls — i.e. exactly
when typing quickly, which matches the reported symptom.

Note that `TCA8418KeyboardBase::trigger()` in `src/input/TCA8418KeyboardBase.cpp` already
does this **correctly** — it reads `TCA8418_REG_KEY_EVENT_A` once per call and guards the
state transitions:

```c
uint8_t k = readRegister(TCA8418_REG_KEY_EVENT_A);
uint8_t key = k & 0x7F;
if (k & 0x80) {
    if (state == Idle)
        pressed(key);
    return;
} else {
    if (state == Held) {
        released();
    }
    state = Idle;
    return;
}
```

The pager override was presumably written to drain multiple events per poll (a reasonable
goal), but indexed the FIFO as if it were an array.

## Root cause 2 — releases are attributed to the wrong key

`released()` takes no key argument and acts on `last_key`, which is set by the most recent
`pressed()`. Because root cause 1 drops release events, and because two keys can legitimately
be down at once (rolling over between keys while typing fast), a release for key A is
routinely processed while `last_key` is already key B — emitting B a second time.

The base class partly contains this with its `if (state == Held)` / `if (state == Idle)`
guards, but the pager override calls `pressed()` and `released()` **unconditionally**,
dropping those guards.

## Root cause 3 — the tap map is indexed by the modifier bitmask, and `char_idx` is dead code

`pressed()` carefully maintains `char_idx` (reset to 0 on a new key, incremented on a repeat
tap within `_TCA8418_MULTI_TAP_THRESHOLD`) and **nothing ever reads it**. `released()`
instead indexes the tap map with the modifier *bitmask*:

```c
queueEvent(TLoraPagerTapMap[last_key][modifierFlag % TLoraPagerTapMod[last_key]]);
```

`modifierFlag` is a bitmask — `0b01` = right shift, `0b10` = sym — and `TLoraPagerTapMod` is
`3` for every key. That works by coincidence for one modifier at a time, but:

* **shift + sym together** gives `modifierFlag == 3`, and `3 % 3 == 0`, so the key silently
  emits its **unshifted lowercase** character instead of the symbol. Another "character that
  doesn't make sense."

`char_idx` and `tap_interval` are then entirely unused. Separately, `tap_interval` is declared
`int32_t` and assigned an unsigned difference, so the `if (tap_interval < 0)` branch is
unreachable except across a `millis()` rollover at ~49.7 days.

## Root cause 4 — `BL_TOGGLE` leaks modifier state

```c
if (TLoraPagerTapMap[last_key][...] == Key::BL_TOGGLE) {
    toggleBacklight();
    return;              // <-- returns without clearing modifierFlag
}
```

The early return skips the `modifierFlag = 0` reset at the end of `released()`, so the sym
modifier stays latched and silently alters the *next* keystroke.

## Reproduction

1. Flash `tlora-pager` (or any env building `TLoraPagerKeyboard.cpp`) onto a T-LoRa Pager.
2. Open any text entry field.
3. Type a moderately long string at normal typing speed, deliberately overlapping keypresses
   slightly as in ordinary touch typing.
4. Observe inserted characters that were never pressed (often repeats of characters typed a
   moment earlier), and dropped keystrokes.
5. Separately: hold sym, then shift, then press a key — observe the lowercase base character
   instead of the symbol.
6. Separately: press the backlight-toggle key while sym is latched — observe the sym modifier
   still applied to the following keystroke.

## Fix

See `0001-tlora-pager-keyboard.patch` in this directory. It is contained entirely to
`src/input/TLoraPagerKeyboard.cpp`:

1. **Drain the FIFO correctly** — read `TCA8418_REG_KEY_EVENT_A` repeatedly (each read pops
   one event) instead of `TCA8418_REG_KEY_EVENT_A + i`, and stop early if the FIFO returns 0.
2. **Restore the base class's state guards** — only `pressed()` from `Idle`, only `released()`
   from `Held`.
3. **Match releases to the key that was actually released** — carry the released key code and
   ignore a release that does not correspond to the key currently held.
4. **Index the tap map by an explicit modifier index** (sym > shift > base) instead of
   `modifierFlag % 3`, fixing the shift+sym case.
5. **Clear modifier state on the `BL_TOGGLE` path.**
6. **Remove the dead `char_idx` / `tap_interval` multi-tap machinery**, which never ran, and
   rename `_TCA8418_MULTI_TAP_THRESHOLD` to `_TCA8418_MODIFIER_TIMEOUT` since with multi-tap
   gone it governs only the shift/sym latch. Its value is unchanged. The
   pager has a full QWERTY matrix; its tap map is modifier-based, not multi-tap. If multi-tap
   is wanted later it should be reintroduced deliberately and actually read.

## Note for the maintainers

The same FIFO-indexing mistake does **not** appear in `TCA8418KeyboardBase::trigger()` or, as
far as we checked, in `TDeckProKeyboard`. It looks specific to the pager override. It may
still be worth auditing any other driver that reads `TCA8418_REG_KEY_EVENT_A` with an offset.

Fixes 1-3 are the ones that address the reported random characters and should be
uncontroversial. Fixes 4-6 are correctness cleanups in the same function and could be split
into a second commit if the maintainers prefer a minimal change.
