# LilyGo T-LoRa Pager — hardware notes

Everything here is read from the Meshtastic variant definition at
`vendor/firmware/variants/esp32s3/tlora-pager/` (`variant.h`, `variant.cpp`,
`pins_arduino.h`, `platformio.ini`) at pin `68bfe015e`, cross-checked against the
drivers that use it. Where something is inferred rather than read, it says so.

**None of this has been verified against a physical board yet.** See
[`STATUS.md`](STATUS.md).

## SoC and memory

| | |
|---|---|
| MCU | ESP32-S3 |
| Flash | 16 MB |
| PSRAM | 8 MB (`BOARD_HAS_PSRAM`) |
| Partition scheme | `default_16MB.csv` |
| Board definition | `t-deck-pro` (shared with the T-Deck Pro) |

### Partition map (`default_16MB.csv`)

| Partition | Type | Offset | Size |
|---|---|---|---|
| `nvs` | data | `0x9000` | 20 KB |
| `otadata` | data | `0xE000` | 8 KB |
| `app0` | app | `0x10000` | 6.25 MiB |
| `app1` | app | `0x650000` | 6.25 MiB |
| `spiffs` | data | `0xC90000` | **3.375 MiB** |
| `coredump` | data | `0xFF0000` | 64 KB |

The data partition is **named** `spiffs` but is **formatted LittleFS**
(`board_build.filesystem = littlefs`); Arduino's `LittleFS.begin()` looks up the
partition labelled `spiffs` by default.

3.375 MiB is the entire budget for Meshtastic's config and node database, PgrOS chat
history, the web portal assets, and the photo gallery. It is not a lot. This is why
PgrOS prefers the SD card for gallery storage when one is present.

## The shared SPI bus — read this before touching SPI

**Three devices sit on SPI2_HOST sharing SCK 35 / MOSI 34 / MISO 33:**

| Device | CS | Notes |
|---|---|---|
| ST7796 display | 38 | DC 37, no reset pin, backlight 42 |
| SX1262 LoRa | 36 | DIO1 14, BUSY 48, RESET 47 |
| SD card | 21 | `SD_SPI_FREQUENCY` 75 MHz — aggressive for a shared bus |

Meshtastic serialises access with a global `spiLock` (`vendor/firmware/src/SPILock.h`),
used as `concurrency::LockGuard g(spiLock);`. Every filesystem, SD and radio operation
takes it.

**Any PgrOS code that drives the display, the SD card, or the radio must hold that
lock.** Flushing pixels without it will toggle the shared bus in the middle of a LoRa
transaction. The failure mode is not a crash — it is intermittent, unreproducible radio
corruption, which is about the worst kind of bug to be handed.

The lock must also be **reentrant**: `concurrency::Lock` is a plain binary semaphore
that deadlocks if the same task takes it twice, and LovyanGFX nests transactions.
Upstream hit this and solved it with `ReentrantSpiLock` in
`vendor/firmware/src/graphics/tftSetup.cpp`; PgrOS uses the same approach in
`src/pgros/hal/Display.cpp`.

## Display

| | |
|---|---|
| Controller | ST7796 |
| Native resolution | 222 × 480 (portrait) |
| Presented as | **480 × 222 landscape**, via `offset_rotation = 3` |
| Offsets | `offset_x = 49`, `offset_y = 0` |
| Bus | SPI2_HOST, 75 MHz write / 16 MHz read |
| Colour | RGB565, `invert = true`, `rgb_order = false` |
| Backlight | GPIO 42, PWM, 44.1 kHz, channel 7 |
| Default brightness | `BRIGHTNESS_DEFAULT` = 130 |

Keep the two resolutions straight: `LGFX_SCREEN_WIDTH/HEIGHT` describe the **native**
panel (222×480); `PGROS_SCREEN_W/H` describe the **post-rotation** surface the UI lays
out against (480×222).

## Keyboard

| | |
|---|---|
| Controller | TCA8418 on I²C, address `0x34` |
| Matrix | 4 rows × 10 columns, 31 mapped keys |
| Interrupt | GPIO 6 (`KB_INT`) |
| Backlight | GPIO 46 (`KB_BL_PIN`), PWM, 1 kHz |
| Reset | XL9555 expander bit 2 |

Two things about the stock driver are worth knowing:

1. **It is polled, not interrupt-driven.** `KbI2cBase::runOnce()` returns 300, so the
   keyboard is scanned every 300 ms and `KB_INT` is used only as a light-sleep wake
   source. The character is emitted on key *release*, so worst-case latency from press
   to delivered character is over 300 ms. That is noticeable while typing.
2. **The stock FIFO readout is buggy** — it reads `KEY_EVENT_A + i` as though the event
   registers were an array, which skips events and decodes stale FIFO slots as
   keystrokes. This is the cause of the random-character problem. Fixed in
   [`patches/upstream/0001-tlora-pager-keyboard/`](../patches/upstream/0001-tlora-pager-keyboard/README.md).

### Key map

The 31 keys map through a 3-column table: base, shift, and sym.

```
q w e r t y u i o p        1 2 3 4 5 6 7 8 9 0
a s d f g h j k l ⏎        * / + - = : ' " @ TAB
      z x c v b n m        _ $ ; ? ! , .
sym            ␣ ⌫         (BL toggle on ␣)
```

Right shift is key 29, sym is key 21 (both 1-indexed).

## Input — rotary encoder

| | |
|---|---|
| A / B | GPIO 40 / 41 |
| Press | GPIO 7 |
| Button | GPIO 0 (`BUTTON_PIN`) |

**The rotary does not emit up/down.** `NodeDB` configures it to emit
`INPUT_BROKER_USER_PRESS` (28) for clockwise and `INPUT_BROKER_ALT_PRESS` (29) for
counter-clockwise. Code that assumes `UP`/`DOWN` will silently do nothing.

It is also gated behind persisted module config:
`moduleConfig.canned_message.updown1_enabled` must be true or `RotaryEncoderImpl::init()`
returns false and the encoder is silently dead. A stale flash can leave that false, so
PgrOS re-asserts it defensively at boot.

## Radio

`variant.h` compiles in SX1262, SX1268, SX1280 and LR1121 support; `initLoRa()` probes
them in order and the SX1262 is the one present on this board.

| | |
|---|---|
| CS / SCK / MISO / MOSI | 36 / 35 / 33 / 34 |
| DIO1 (IRQ) | 14 |
| BUSY | 48 |
| RESET | 47 |
| TCXO | 3.0 V via DIO3 |
| RF switch | DIO2 |

## GPS

| | |
|---|---|
| UART RX / TX | 4 / 12 |
| PPS | 13 |
| Baud | 38400 |
| Power enable | XL9555 expander bit 4 |
| Reset | XL9555 expander bit 7 |

Note that GPS probing happens on the **main loop**, not in `setup()`, and
`GPS::probe()` contains several hundred milliseconds of `delay()` while it tries baud
rates. Anything sharing the main loop will visibly stall during early GPS acquisition.
This is a large part of why the PgrOS UI runs on its own task.

## Audio and haptics

| | |
|---|---|
| Codec | ES8311 over I²S |
| BCK / WS / DOUT / DIN / MCLK | 11 / 18 / 45 / 17 / 10 |
| Amplifier enable | XL9555 expander bit 1 (`EXPANDS_AMP_EN`) |
| Haptic driver | DRV2605 on I²C |
| Haptic enable | XL9555 expander bit 0 (`EXPANDS_DRV_EN`) |

The amplifier enable is the important one for PgrOS's silent-boot requirement: an amp
that powers up unmuted produces an audible pop. `Silence::muteAmplifierEarly()` drives
it low during stage 0, before any audio code can run.

## Power

| | |
|---|---|
| Charger | BQ25896 |
| Fuel gauge | BQ27220, 1500 mAh design capacity |
| RTC | PCF85063 @ `0x51` |
| Power saving | `USE_POWERSAVE`, `SLEEP_TIME` 120 s |

`USE_POWERSAVE` causes `NodeDB` to default `screen_on_secs` to 30 and
`is_power_saving` to true.

## XL9555 I/O expander map

| Bit | Function |
|---|---|
| 0 | Haptic driver enable |
| 1 | **Audio amplifier enable** |
| 2 | Keyboard reset |
| 3 | LoRa enable |
| 4 | GPS enable |
| 5 | NFC enable |
| 7 | GPS reset |
| 8 | Keyboard enable |
| 9 | GPIO enable |
| 10 | SD card detect |
| 11 | SD card pull-up enable |
| 12 | SD card enable |

The expander is brought up in `earlyInitVariant()`, which runs **before**
`consoleInit()`. A `LOG_*` call in that function crashes the device — upstream has a
comment saying exactly that.

## Other peripherals

| | |
|---|---|
| IMU | BHI260AP (gyroscope) |
| NFC | ST25R3916, INT 5, CS 39 |
| I²C | SDA 3, SCL 2, `I2C_NO_RESCAN` set |

The BHI260AP driver is the source of a build break when the stock screen is compiled
out: `BHI260APSensor.cpp` writes `screen->steps`, and `steps` exists only on the real
`Screen` class, not the no-op stub. PgrOS therefore builds with
`-D MESHTASTIC_EXCLUDE_ACCELEROMETER=1`. If step counting is wanted later, the fix is
to add `steps` to the stub rather than to re-enable the sensor blindly.

## Flashing

The T-LoRa Pager needs DFU mode: **hold BOOT, tap RESET, release BOOT**, then

```bash
./scripts/build.ps1 -Target upload -Port COM7
```
