#ifdef PGROS

#include "hal/Display.h"

#include "configuration.h"

#include "SPILock.h"
#include "concurrency/LockGuard.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include <lvgl.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace pgros
{

Display display;

// ---------------------------------------------------------------------------
// Panel
//
// Values are the ones meshtastic/device-ui uses for this board, which are in
// turn the ones LilyGo ship. Two deliberate deviations, both because we drive
// LovyanGFX directly instead of through device-ui's wrapper:
//
//   use_lock   = true   let LovyanGFX serialise its own nested transactions
//   bus_shared = true   the SX1262 and the SD card are on this bus too
//
// Those only protect us from ourselves. The cross-device protection is spiLock,
// taken in flushCb(); see the header comment.
// ---------------------------------------------------------------------------
class LGFX_TLoraPager : public lgfx::LGFX_Device
{
    lgfx::Panel_ST7796 _panel;
    lgfx::Bus_SPI _bus;
    lgfx::Light_PWM _light;

  public:
    LGFX_TLoraPager()
    {
        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = SPI_FREQUENCY;      // 75 MHz, from variant.h
            cfg.freq_read = SPI_READ_FREQUENCY;  // 16 MHz
            cfg.spi_3wire = false;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = ST7796_SCK;
            cfg.pin_mosi = ST7796_SDA;
            cfg.pin_miso = ST7796_MISO;
            cfg.pin_dc = ST7796_RS;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = TFT_CS;
            cfg.pin_rst = ST7796_RESET; // -1, no reset line on this board
            cfg.pin_busy = ST7796_BUSY; // -1
            // Native portrait dimensions; offset_rotation turns it landscape.
            cfg.panel_width = TFT_WIDTH;   // 222
            cfg.panel_height = TFT_HEIGHT; // 480
            cfg.offset_x = TFT_OFFSET_X;   // 49
            cfg.offset_y = TFT_OFFSET_Y;   // 0
            cfg.offset_rotation = TFT_OFFSET_ROTATION; // 3
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits = 1;
            cfg.readable = true;
            cfg.invert = true;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = true;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl = ST7796_BL; // 42
            cfg.invert = false;
            cfg.freq = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};

static LGFX_TLoraPager lgfx;

// ---------------------------------------------------------------------------
// Reentrant SPI lock
//
// Meshtastic's concurrency::Lock is a plain binary semaphore: taking it twice
// from the same task deadlocks. LovyanGFX nests transactions, so a naive
// LockGuard around a flush will hang the UI task the first time it happens.
//
// Track owner and depth, and only touch the real lock at the outermost level.
// Upstream reached the same conclusion; see ReentrantSpiLock in
// src/graphics/tftSetup.cpp.
// ---------------------------------------------------------------------------
namespace
{
TaskHandle_t spiOwner = nullptr;
uint16_t spiDepth = 0;

void spiAcquire()
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (spiDepth && spiOwner == self) {
        spiDepth++;
        return;
    }
    spiLock->lock();
    spiOwner = self;
    spiDepth = 1;
}

void spiRelease()
{
    if (spiDepth == 0)
        return; // unbalanced release; never let it underflow
    if (--spiDepth == 0) {
        spiOwner = nullptr;
        spiLock->unlock();
    }
}
} // namespace

// ---------------------------------------------------------------------------
// Panel bring-up
// ---------------------------------------------------------------------------

bool Display::beginPanel()
{
    concurrency::LockGuard g(spiLock);

    if (!lgfx.init()) {
        LOG_ERROR("PgrOS: LGFX init failed");
        return false;
    }

    // Backlight stays at zero. Nothing is drawn yet, and lighting an
    // uninitialised framebuffer shows a white flash on this panel.
    lgfx.setBrightness(0);
    mBrightness = 0;
    mOn = true;

    lgfx.setRotation(0); // rotation is baked into offset_rotation
    lgfx.fillScreen(0x0000);

    LOG_INFO("PgrOS: panel up, %dx%d", (int)lgfx.width(), (int)lgfx.height());
    return true;
}

void Display::drawSplash()
{
    concurrency::LockGuard g(spiLock);

    const int w = lgfx.width();
    const int h = lgfx.height();

    // Near-black rather than true black: this panel bands visibly on #000, and
    // the same charcoal is the UI background, so the handoff to LVGL is seamless.
    lgfx.fillScreen(lgfx.color565(0x12, 0x14, 0x18));

    // Wordmark, centred. Drawn with the built-in font so there is no font data
    // to load and no filesystem dependency -- neither exists this early.
    lgfx.setTextDatum(middle_center);

    lgfx.setTextColor(lgfx.color565(0xE8, 0xEA, 0xED));
    lgfx.setTextSize(4);
    lgfx.drawString("PgrOS", w / 2, h / 2 - 14);

    lgfx.setTextColor(lgfx.color565(0x6E, 0x9E, 0xFF)); // accent
    lgfx.setTextSize(1);
    lgfx.drawString("MESHTASTIC", w / 2, h / 2 + 24);

    // A thin accent rule under the wordmark gives the splash a deliberate look
    // rather than "text on a black screen".
    const int ruleW = 120;
    lgfx.fillRect((w - ruleW) / 2, h / 2 + 12, ruleW, 2, lgfx.color565(0x6E, 0x9E, 0xFF));
}

void Display::rampBacklight(uint8_t target, uint16_t ms)
{
    // Fade up over a handful of steps. A hard 0->130 step reads as a flash;
    // ~16 steps is smooth without being slow enough to notice as a delay.
    const uint8_t steps = 16;
    const uint16_t stepDelay = ms / steps;

    for (uint8_t i = 1; i <= steps; ++i) {
        uint8_t level = (uint16_t)target * i / steps;
        {
            concurrency::LockGuard g(spiLock);
            lgfx.setBrightness(level);
        }
        if (stepDelay)
            vTaskDelay(pdMS_TO_TICKS(stepDelay));
    }
    mBrightness = target;
    mOn = true;
}

// ---------------------------------------------------------------------------
// LVGL
// ---------------------------------------------------------------------------

static lv_display_t *lvDisplay = nullptr;
static lv_color_t *lvBuf1 = nullptr;
static lv_color_t *lvBuf2 = nullptr;

// LVGL asks the platform for the current time in ms.
static uint32_t lvTickCb()
{
    return millis();
}

void Display::flushCb(lv_display_t *disp, const void *areaV, unsigned char *px)
{
    const lv_area_t *area = (const lv_area_t *)areaV;
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;

    uint32_t t0 = micros();

    // LVGL renders RGB565 little-endian; the ST7796 expects big-endian over the
    // wire. Swapping here is cheaper than making LVGL render swapped, because
    // this runs on already-contiguous memory.
    lv_draw_sw_rgb565_swap(px, w * h);

    {
        // The ONLY place pixels touch the shared bus. Held around the transfer
        // and nothing else -- holding it across LVGL's render would stall the
        // LoRa stack for tens of ms per frame.
        spiAcquire();
        lgfx.startWrite();
        lgfx.setAddrWindow(area->x1, area->y1, w, h);
        lgfx.writePixels((uint16_t *)px, w * h, false);
        lgfx.endWrite();
        spiRelease();
    }

    display.mLastFlushUs = (uint16_t)min<uint32_t>(micros() - t0, 65535);
    display.mFlushCount++;

    lv_display_flush_ready(disp);
}

bool Display::beginLvgl()
{
    lv_init();
    lv_tick_set_cb(lvTickCb);

    lvDisplay = lv_display_create(PGROS_SCREEN_W, PGROS_SCREEN_H);
    if (!lvDisplay) {
        LOG_ERROR("PgrOS: lv_display_create failed");
        return false;
    }

    // Quarter-screen partial buffer, the strategy device-ui proved on this
    // panel. Two of them so LVGL can render one while the other is in flight.
    const size_t pixels = (size_t)PGROS_SCREEN_W * PGROS_SCREEN_H / 4;
    const size_t bytes = pixels * sizeof(lv_color_t);

    // 32-byte alignment is REQUIRED, not cosmetic: plain ps_malloc() and
    // heap_caps_malloc(MALLOC_CAP_SPIRAM) both crash on this DMA path.
    lvBuf1 = (lv_color_t *)heap_caps_aligned_alloc(32, bytes, MALLOC_CAP_SPIRAM);
    lvBuf2 = (lv_color_t *)heap_caps_aligned_alloc(32, bytes, MALLOC_CAP_SPIRAM);

    size_t useBytes = bytes;
    if (!lvBuf1) {
        // Fall back to a much smaller internal-RAM buffer rather than failing
        // to boot. The UI will be slower but the device stays usable.
        LOG_WARN("PgrOS: PSRAM draw buffer failed; falling back to internal RAM");
        useBytes = (size_t)PGROS_SCREEN_W * 24 * sizeof(lv_color_t);
        lvBuf1 = (lv_color_t *)heap_caps_aligned_alloc(32, useBytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (lvBuf2) {
            heap_caps_free(lvBuf2);
            lvBuf2 = nullptr;
        }
        if (!lvBuf1) {
            LOG_ERROR("PgrOS: no draw buffer available");
            return false;
        }
    }

    lv_display_set_buffers(lvDisplay, lvBuf1, lvBuf2, useBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(lvDisplay, (lv_display_flush_cb_t)&Display::flushCb);

    LOG_INFO("PgrOS: LVGL up, %u byte buffers%s", (unsigned)useBytes, lvBuf2 ? " (double)" : " (single)");
    return true;
}

// ---------------------------------------------------------------------------
// Power / brightness
// ---------------------------------------------------------------------------

void Display::setBrightness(uint8_t level)
{
    if (level == mBrightness)
        return;

    // Ramp rather than step, in both directions.
    const int8_t dir = (level > mBrightness) ? 1 : -1;
    while (mBrightness != level) {
        int next = (int)mBrightness + dir * 8;
        if ((dir > 0 && next > level) || (dir < 0 && next < level))
            next = level;
        mBrightness = (uint8_t)next;
        {
            concurrency::LockGuard g(spiLock);
            lgfx.setBrightness(mBrightness);
        }
        vTaskDelay(pdMS_TO_TICKS(4));
    }
}

void Display::setOn(bool on)
{
    if (on == mOn)
        return;

    if (on) {
        {
            concurrency::LockGuard g(spiLock);
            lgfx.wakeup();
        }
        mOn = true;
        // Redraw happens before the light comes back, same rule as boot.
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(lvDisplay);
        setBrightness(mBrightness ? mBrightness : BRIGHTNESS_DEFAULT);
    } else {
        setBrightness(0);
        concurrency::LockGuard g(spiLock);
        lgfx.sleep();
        mOn = false;
    }
}

void Display::prepareForDeepSleep()
{
    // PowerFSM would normally call screen->doDeepSleep(), but that is a no-op
    // stub in this build, so blanking the panel is our job.
    setBrightness(0);
    concurrency::LockGuard g(spiLock);
    lgfx.sleep();
    lgfx.powerSaveOn();
    mOn = false;
}

uint16_t Display::width() const
{
    return PGROS_SCREEN_W;
}

uint16_t Display::height() const
{
    return PGROS_SCREEN_H;
}

} // namespace pgros

#endif // PGROS
