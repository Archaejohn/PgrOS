# Upstream improvement 0002 — I²C keyboards are polled at a flat 300 ms, which is felt as typing lag

**Affected file:** `src/input/kbI2cBase.cpp`, `src/input/kbI2cBase.h`
**Affected hardware:** every board using `KbI2cBase` — T-LoRa Pager, T-Deck, T-Deck Pro,
BBQ10/Q10, MPR121, CardKB, ThinkNode M9
**Found against:** `meshtastic/firmware` @ `68bfe015e6ab9ec2ab8f1657066898b7880eaf63` (develop, 2026-08-20)
**Status:** fix written and applied in PgrOS as `0002-keyboard-poll-latency.patch`
**Type:** responsiveness improvement, not a correctness bug

## Symptom

Typing on a device with a physical I²C keyboard feels laggy — characters appear a
noticeable beat after the key is pressed. It is most obvious when typing at speed,
where the interface appears to be "catching up" with the user.

## Cause

`KbI2cBase::runOnce()` ends with an unconditional:

```c
    return 300;
```

`OSThread::runOnce()` returns the number of milliseconds until the next invocation, so
the keyboard controller is read **at most once every 300 ms**, regardless of what the
user is doing.

Two things compound it:

1. **The character is emitted on key *release*, not press.** For the TCA8418 drivers,
   `pressed()` only latches state; `queueEvent()` happens in `released()`. So the press
   event and the release event each need their own poll, and the delay lands *between
   every letter* rather than once at the start of a burst.
2. **`KB_INT` is not used for this.** On the T-LoRa Pager, `KB_INT` (GPIO 6) is only
   `pinMode(INPUT_PULLUP)`'d in the variant and consumed as a light-sleep wake source in
   `src/sleep.cpp`. There is no `attachInterrupt` on it for this board, so a pending key
   event sits in the controller's FIFO until the next scheduled poll.

Worst-case press-to-character latency is therefore over 300 ms, and typical latency
across a burst of typing is a substantial fraction of that per character.

## Fix

Poll adaptively instead of at a fixed rate. Typing is bursty: poll quickly for a short
window after each key event, and fall back to the original idle rate when nothing is
happening.

```c
static constexpr int32_t kPollIdleMs = 300;
static constexpr int32_t kPollActiveMs = 20;
static constexpr uint32_t kActiveWindowMs = 750;

int32_t KbI2cBase::pollInterval() const
{
    if (lastKeyMs && (millis() - lastKeyMs) < kActiveWindowMs)
        return kPollActiveMs;
    return kPollIdleMs;
}
```

`lastKeyMs` is stamped by a one-line `notifyKey()` wrapper, and the six existing
`this->notifyObservers(&e)` call sites in `runOnce()` are routed through it. The final
`return 300;` becomes `return pollInterval();`.

### Cost

**An idle device is unchanged** — still one I²C transaction every 300 ms. The faster
rate only applies for 750 ms after an actual key event, i.e. only while someone is
typing, when the device is awake and the user is actively engaged anyway.

The bound on extra work is ~37 additional I²C reads per second during a typing burst.
Each is a two-byte register read on a bus that is otherwise idle between sensor polls.

### Result

Press-to-character latency during a burst of typing drops from up to ~300 ms to ~20-40 ms.
The first keystroke after an idle period still costs up to 300 ms, which is not
perceptible because there is nothing to compare it against.

## The better fix, deliberately not done here

The genuinely correct solution is to make the keyboard interrupt-driven:
`attachInterrupt(KB_INT, ...)` calling `inputBroker->requestPollSoon(this)`. **The
plumbing for this already exists and is already used** — `InputBroker::requestPollSoon()`
(`src/input/InputBroker.cpp`) is ISR-safe, feeds a dedicated `input-pollSoon` task, and
is exactly how `RotaryEncoderImpl` gets its low latency.

It was not done in this patch because `KbI2cBase` would have to implement
`InputPollable`, and `KbI2cBase` is shared by seven or more boards with different
interrupt wiring (several of which do not define `KB_INT` at all). That is a
substantially larger and riskier change to make blind, without hardware for each of
those boards to test on.

The adaptive interval here is board-agnostic, cannot regress any board's behaviour
beyond the bounded extra polling described above, and captures most of the benefit. If
maintainers would prefer the interrupt-driven version, it is a reasonable follow-up and
we are happy to write it.

## Testing status

**Compiled, not yet run on hardware.** PgrOS has no physical pager available at the time
of writing, so the latency improvement is reasoned from the code rather than measured.
The change is small and its failure mode is benign (polling more often than necessary),
but it deserves a real measurement before merging. We will follow up with numbers once
we have a device.
