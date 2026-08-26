# Upstream bug report 0003 — `BHI260APSensor.cpp` fails to compile when the screen is excluded

**Affected file:** `src/motion/BHI260APSensor.cpp`
**Affected hardware:** any board defining `HAS_BHI260AP` — LilyGo T-LoRa Pager, T-Watch S3, and others
**Found against:** `meshtastic/firmware` @ `68bfe015e6ab9ec2ab8f1657066898b7880eaf63` (develop, 2026-08-20)
**Status:** fix written and applied in PgrOS as `0003-bhi260ap-screen-steps.patch`
**Type:** build break (portability), not a runtime bug

## Symptom

Building a `HAS_BHI260AP` board with the stock UI compiled out fails:

```
src/motion/BHI260APSensor.cpp:67:13: error: 'screen' was not declared in this scope
```

or, if `screen` is visible, an error that `graphics::Screen` has no member named `steps`.

## Cause

`BHI260APSensor::runOnce()` writes the step count straight into the screen object:

```c
if (stepCounter->hasUpdated()) {
    steps = stepCounter->getStepCount();
    LOG_WARN("Step count updated: %u", steps);
    if (screen)
        screen->steps = steps;     // <-- unguarded
}
```

`steps` is a member of the **real** `graphics::Screen` only (`src/graphics/Screen.h`, in the
`#else` half of the `#if !HAS_SCREEN` split). When `HAS_SCREEN` is 0, `Screen.h` substitutes
a no-op stub class, and that stub has no `steps` member — nor any of the other public data
members.

The file's only compile guard is:

```c
#if !defined(ARCH_STM32WL) && !MESHTASTIC_EXCLUDE_I2C && defined(HAS_BHI260AP) && __has_include(<SensorBHI260AP.hpp>)
```

Note what is **not** in that condition: `HAS_SCREEN`, and `MESHTASTIC_EXCLUDE_SCREEN`. It also
does not test `MESHTASTIC_EXCLUDE_ACCELEROMETER`, so setting that flag — the obvious thing to
reach for — does **not** avoid the problem. The file is compiled regardless, and the write to
`screen->steps` is reached regardless.

So any configuration that pairs a BHI260AP with `MESHTASTIC_EXCLUDE_SCREEN` fails to build.
That combination is not exotic: it is what every headless build and every custom-UI build of
an affected board does. It is presumably unreported only because nobody has yet built one of
these boards without BaseUI.

The `IF_SCREEN(...)` macro in `src/meshUtils.h` exists for exactly this and is used correctly
in many comparable places (`TextMessageModule.cpp`, `SystemCommandsModule.cpp`,
`AdminModule.cpp`). This site simply predates it or was missed.

## Reproduction

Build any `HAS_BHI260AP` variant with `-D MESHTASTIC_EXCLUDE_SCREEN=1`. For example, add to
`variants/esp32s3/tlora-pager/platformio.ini`:

```ini
[env:tlora-pager-headless]
extends = env:tlora-pager
build_flags = ${env:tlora-pager.build_flags} -D MESHTASTIC_EXCLUDE_SCREEN=1
```

then `pio run -e tlora-pager-headless`.

## Fix

Guard the write, which is the minimal change and matches how the rest of the tree handles the
same situation:

```c
#if HAS_SCREEN
        if (screen)
            screen->steps = steps;
#endif
```

See `0003-bhi260ap-screen-steps.patch`. The step count is still computed and logged; only the
handoff to the UI is compiled out, which is correct — there is no UI to hand it to.

`IF_SCREEN(screen->steps = steps);` would be equally acceptable and slightly more idiomatic;
we used the explicit `#if` because the surrounding code already tests `screen` directly and
the intent is clearer to a reader who has just hit the build error.

### An alternative worth considering

Adding `uint32_t steps = 0;` to the no-op `Screen` stub in `Screen.h` would fix this and any
future site that touches it, at the cost of four bytes. Given the stub already carries a full
set of no-op methods specifically so callers do not need `#if` guards, having it carry the
data members too would be consistent. That is a maintainer call; the patch here takes the
conservative route.

## Testing status

**Compiles; not run on hardware.** PgrOS builds cleanly with this patch applied
(`MESHTASTIC_EXCLUDE_SCREEN=1`, `HAS_BHI260AP` defined, T-LoRa Pager). The change is
compile-time only and cannot alter behaviour in any build where `HAS_SCREEN` is 1, so runtime
risk is nil for existing configurations.
