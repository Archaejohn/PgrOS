# Integration hook 0002 — double-tap SYM emits the emote-list event

**Affected files:**
`src/input/TCA8418KeyboardBase.h`, `src/input/TLoraPagerKeyboard.{cpp,h}`, `src/input/kbI2cBase.cpp`
**Type:** PgrOS feature hook. **Not** an upstream bug, and not proposed as one.

## Why this cannot live in PgrOS

`TLoraPagerKeyboard::pressed()` handles modifier keys entirely inside the driver:

```c
if (isModifierKey(idx)) {
    modifierHeld |= modifierBit(idx);
    updateModifierFlag(idx);
    last_modifier_time = now;
    return;                      // never reaches the event queue
}
```

SYM latches a flag and returns. It never becomes an `InputEvent`, so nothing
above the driver can observe that the key exists, let alone that it was pressed
twice. Detecting the gesture requires a change at this level; there is no seam
further up.

## What the patch does

Adds `EMOTE_LIST = 0x8F` to `TCA8418Key`, emits it from `pressed()` when a second
SYM tap arrives within 450 ms, and maps it in `kbI2cBase` the way every other
non-character key in that file is mapped.

`0x8F` is not a new number. It is upstream's own `INPUT_BROKER_MSG_EMOTE_LIST`,
which `kbI2cBase.cpp` already produces for other keyboard types as `fn+e`:

```c
case 0x8F: // fn+e      INPUT_BROKER_MSG_EMOTE_LIST
```

The TCA8418 path simply had no route to it. So this adds a producer on one more
keyboard rather than inventing a private protocol.

The first tap has already latched symbol mode by the time the second arrives, so
the gesture clears that latch before emitting. Without it, dismissing the picker
would leave the keyboard typing punctuation.

## Why it is not an upstream patch

Upstream has no consumer for `INPUT_BROKER_MSG_EMOTE_LIST` — no screen or module
in the tree reacts to it. Sending an upstream PR that adds a second producer for
an event nothing handles would be noise. If a stock-UI emoji picker ever lands,
this becomes a three-line addition worth proposing then.

PgrOS supplies the consumer: `key::Emoji` in `src/pgros/hal/Keyboard.cpp`, and
the picker in `ConversationApp`.

## Behaviour

| Gesture | Result |
|---|---|
| SYM once | symbol mode, as before |
| SYM twice within 450 ms | emoji picker opens |
| SYM twice again | picker closes, back to the keyboard |
| SYM held | hold-to-symbol, as before |

450 ms is long enough to be comfortable and short enough that two deliberate
symbol-mode taps are not mistaken for it — and two such taps are a no-op anyway,
since the second merely unlatches the first.
