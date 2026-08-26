#pragma once
//
// Panel + LVGL binding for the T-LoRa Pager's ST7796.
//
// ---------------------------------------------------------------------------
// THE SPI BUS IS SHARED. This is the most dangerous thing in the firmware.
// ---------------------------------------------------------------------------
//
// On this board SPI2_HOST carries three devices on the same SCK/MOSI/MISO
// (35/34/33):
//
//     ST7796 display   CS 38
//     SX1262 LoRa      CS 36
//     SD card          CS 21
//
// Meshtastic serialises access with a global `spiLock` (src/SPILock.h), and
// every filesystem, SD and radio operation takes it. If PgrOS flushes pixels
// without holding that lock, it will drive CS and clock lines in the middle of
// a LoRa transaction and corrupt it -- producing exactly the kind of
// intermittent, impossible-to-reproduce radio fault that is miserable to debug.
//
// So: EVERY flush holds spiLock, and it is taken around the transfer only --
// never around LVGL's render or timer work, which would block the radio for
// tens of milliseconds per frame. Upstream learned this the hard way; see the
// comment above ReentrantSpiLock in src/graphics/tftSetup.cpp.
//
// The lock must be REENTRANT, because Meshtastic's concurrency::Lock is a plain
// binary semaphore that deadlocks if the same task takes it twice, and
// LovyanGFX can nest transactions.

#include <stdint.h>

struct _lv_display_t;
typedef struct _lv_display_t lv_display_t;

namespace pgros {

class Display
{
  public:
    // Panel bring-up only: bus, panel config, rotation, backlight left OFF.
    // Called from earlyBoot() before the filesystem exists.
    bool beginPanel();

    // Paint the boot splash directly through LovyanGFX, with no LVGL involved.
    // LVGL is not initialised this early, and we want pixels immediately.
    void drawSplash();

    // Ramp the backlight from 0 to `target` over `ms`, so the panel fades up
    // onto content rather than snapping on. Blocking, but only for `ms`.
    void rampBacklight(uint8_t target, uint16_t ms = 180);

    // Initialise LVGL: display driver, draw buffers, tick source.
    //
    // Buffers come from PSRAM via heap_caps_aligned_alloc(32, ...). Note that
    // plain ps_malloc()/heap_caps_malloc(MALLOC_CAP_SPIRAM) are known to crash
    // with DMA on this path -- the 32-byte alignment is required, not
    // decorative. Uses a partial (quarter-screen) buffer in
    // LV_DISPLAY_RENDER_MODE_PARTIAL, matching the proven device-ui strategy.
    bool beginLvgl();

    // Brightness, 0..255. Ramped internally, never stepped.
    void setBrightness(uint8_t level);
    uint8_t brightness() const { return mBrightness; }

    // Panel power. Blanking on inactivity is PgrOS's job: Meshtastic's PowerFSM
    // calls screen->setOn(false), but with the stock UI compiled out that is a
    // no-op stub, so nothing would ever turn the panel off.
    void setOn(bool on);
    bool isOn() const { return mOn; }

    // Put the panel into its lowest-power state before deep sleep.
    void prepareForDeepSleep();

    uint16_t width() const;
    uint16_t height() const;

    // Diagnostics: how long the last flush held spiLock. If this climbs, the
    // radio is being starved.
    uint16_t lastFlushUs() const { return mLastFlushUs; }
    uint32_t flushCount() const { return mFlushCount; }

  private:
    // LVGL flush callback. Takes spiLock around the DMA transfer only.
    static void flushCb(lv_display_t *disp, const void *area, unsigned char *px);

    bool mOn = false;
    uint8_t mBrightness = 0;
    uint16_t mLastFlushUs = 0;
    uint32_t mFlushCount = 0;
};

extern Display display;

} // namespace pgros
