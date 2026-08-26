#pragma once
//
// PgrOS visual language.
//
// The panel is 480x222 -- wide and short, closer to a car dashboard than a
// phone. Two consequences drive every choice here:
//
//   1. Vertical space is scarce. There is room for roughly five list rows or
//      four message bubbles. Chrome has to be thin; a 40px title bar would cost
//      a fifth of the screen. The status bar is 22px and there is no title bar
//      -- the screen title lives inside the status bar.
//
//   2. Horizontal space is plentiful. Message bubbles cap at 70% width so the
//      sender/timestamp column stays readable, and list rows can afford a
//      leading avatar plus a trailing timestamp.
//
// Colours are defined once, here, as a small semantic palette rather than raw
// hex scattered through the apps. Dark is the default: this is a device used
// outdoors and at night, and a mostly-black screen is both easier to read in
// the dark and cheaper on an always-on panel.

#include <stdint.h>

struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;
struct _lv_font_t;
typedef struct _lv_font_t lv_font_t;

namespace pgros {

// 16-bit RGB565 is what the panel takes; LVGL converts from 24-bit for us.
typedef uint32_t Color;

struct Palette {
    Color bg;          // screen background
    Color surface;     // cards, list rows, the status bar
    Color surfaceAlt;  // pressed/selected row
    Color border;      // hairlines and dividers
    Color text;        // primary text
    Color textDim;     // timestamps, secondary labels
    Color textFaint;   // placeholder text
    Color accent;      // brand / focus ring / primary action
    Color accentText;  // text on top of accent
    Color bubbleOut;   // our own message bubble
    Color bubbleIn;    // someone else's message bubble
    Color ok;          // delivered, connected, good signal
    Color warn;        // queued, weak signal
    Color error;       // failed, disconnected
};

// Named metrics, so spacing stays consistent and is tunable in one place.
namespace metrics {
static constexpr int16_t screenW = 480;
static constexpr int16_t screenH = 222;

static constexpr int16_t statusBarH = 22;
static constexpr int16_t contentH = screenH - statusBarH;

static constexpr int16_t padXs = 2;
static constexpr int16_t padS = 4;
static constexpr int16_t padM = 8;
static constexpr int16_t padL = 12;

static constexpr int16_t radiusS = 4;
static constexpr int16_t radiusM = 8;
static constexpr int16_t radiusBubble = 10;

static constexpr int16_t listRowH = 38;    // 5 rows fit in the content area
static constexpr int16_t bubbleMaxPct = 70; // % of width a bubble may occupy
static constexpr int16_t composerH = 34;

static constexpr int16_t iconS = 14;
static constexpr int16_t avatarS = 26;
} // namespace metrics

class Theme
{
  public:
    // Installs the LVGL theme and builds the shared styles. Call once, after
    // lv_init() and before any app is created.
    void begin();

    // Switch palette at runtime. Rebuilds styles and invalidates the screen.
    void setDark(bool dark);
    bool isDark() const { return mDark; }

    const Palette &colors() const { return mPalette; }

    // --- fonts -----------------------------------------------------------
    // Three sizes is all a 222px-tall panel can justify.
    const lv_font_t *fontSmall() const;  // timestamps, status bar
    const lv_font_t *fontBody() const;   // message text, list titles
    const lv_font_t *fontLarge() const;  // clock, headings

    // --- shared style application ----------------------------------------
    // Applied by apps so widgets look the same everywhere without each app
    // hand-setting a dozen local styles.
    void styleScreen(lv_obj_t *obj);
    void styleCard(lv_obj_t *obj);
    void styleListRow(lv_obj_t *obj);
    void styleBubble(lv_obj_t *obj, bool outbound);
    void styleButton(lv_obj_t *obj, bool primary = false);
    void styleTextInput(lv_obj_t *obj);
    void styleLabelDim(lv_obj_t *obj);

    // Colour for a delivery state, so the mapping lives in one place.
    Color statusColor(uint8_t msgStatus) const;

    // Colour for a signal strength, 0..4 bars.
    Color signalColor(uint8_t bars) const;

  private:
    void buildPalette();

    bool mDark = true;
    Palette mPalette;
};

extern Theme theme;

} // namespace pgros
