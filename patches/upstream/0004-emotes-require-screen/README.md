# Upstream bug report 0004 — the emoji table is unavailable when the screen is excluded

**Affected file:** `src/graphics/emotes.cpp`
**Affected hardware:** any board built with `MESHTASTIC_EXCLUDE_SCREEN` / `HAS_SCREEN == 0`
**Found against:** `meshtastic/firmware` @ `68bfe015e6ab9ec2ab8f1657066898b7880eaf63` (develop, 2026-08-20)
**Status:** fix written and applied in PgrOS as `0004-emotes-require-screen.patch`
**Type:** portability / layering, not a runtime bug on stock builds

## Symptom

A firmware built without the stock UI cannot resolve a single emoji it receives
over the mesh — not to draw it, and not even to name it:

```
undefined reference to `graphics::emotes'
undefined reference to `graphics::numEmotes'
```

## Cause

`emotes.cpp` wraps its entire contents in a display guard:

```c
#include "configuration.h"
#if HAS_SCREEN
#include "emotes.h"
...
#endif
```

The file holds two very different things:

1. **`emotes[]` and `numEmotes`** — a table of UTF-8 code points paired with
   16×16 bitmaps. Pure data. It knows nothing about a display.
2. **The bitmaps themselves** — also pure data.

Neither depends on `OLEDDisplay`, on `graphics::Screen`, or on anything the
`HAS_SCREEN` split changes. The rendering code that *does* need a display lives
in `EmoteRenderer.cpp`, which is guarded separately and correctly.

So the guard is attached to the wrong layer. It excludes the data on the grounds
that the stock renderer is the only thing that could want it — which stops being
true the moment a firmware brings its own UI, and is not true even for a
headless build that merely wants to recognise an emoji in a received message.

## Consequence

Any alternative UI on a `HAS_SCREEN == 0` build has to either vendor its own copy
of the table — 146 entries and ~400 lines of bitmaps that then drift out of step
with upstream and with the phone app's tapback set — or ship a full emoji font,
which for a monochrome 16px glyph is megabytes to replace kilobytes that are
already in the tree.

## Fix

Widen the guard so the data can be requested explicitly:

```c
#if HAS_SCREEN || defined(MESHTASTIC_INCLUDE_EMOTES)
```

This is deliberately additive. `HAS_SCREEN` builds are unaffected, the flash
saving for screenless builds that do not ask for the table is preserved, and a
build that wants the data opts in with one define.

An arguably cleaner fix is to split the file — data in `emotes.cpp`, guard only
the renderer — but that changes the shape of the tree for every consumer, and
this patch aims to be the smallest change that removes the coupling.

## How PgrOS uses it

PgrOS builds with `-D MESHTASTIC_EXCLUDE_SCREEN=1 -D MESHTASTIC_INCLUDE_EMOTES=1`
and wraps the table in an LVGL fallback font (`src/pgros/ui/EmojiFont.cpp`), so
emoji render in message bubbles and feed the on-device emoji picker. Same data,
same code points, a different renderer.
