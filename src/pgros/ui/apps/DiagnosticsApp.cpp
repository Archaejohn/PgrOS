#ifdef PGROS

#include "ui/apps/DiagnosticsApp.h"

#include "configuration.h"

#include "FSCommon.h"
#include "core/EventBus.h"
#include "core/MeshBridge.h"
#include "core/Panic.h"
#include "core/Service.h"
#include "hal/Display.h"
#include "hal/Keyboard.h"
#include "store/ChatStore.h"
#include "ui/Shell.h"
#include "ui/Theme.h"

#include <lvgl.h>
#include <stdarg.h>
#include <stdio.h>

namespace pgros
{

DiagnosticsApp diagnosticsApp;

// Row indices, so refresh() and the build order cannot drift apart.
enum : uint8_t {
    kUptime = 0,
    kHeap,
    kHeapMin,
    kPsram,
    kCpu,
    kFlash,
    kChat,
    kFrames,
    kFrameMs,
    kFlush,
    kDropped,
    kMesh,
    kNeighbours,
    kAirtime,
    kLineCount
};

void DiagnosticsApp::addLine(lv_obj_t *parent, const char *label)
{
    if (mCount >= kMaxLines)
        return;

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, metrics::screenW - metrics::padL * 2, 15);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *k = lv_label_create(row);
    lv_obj_set_style_text_font(k, theme.fontSmall(), 0);
    lv_obj_set_style_text_color(k, lv_color_hex(theme.colors().textDim), 0);
    lv_label_set_text(k, label);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_style_text_font(v, theme.fontSmall(), 0);
    lv_obj_set_style_text_color(v, lv_color_hex(theme.colors().text), 0);
    lv_label_set_text(v, "-");
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);

    mKey[mCount] = k;
    mVal[mCount] = v;
    mCount++;
}

void DiagnosticsApp::setValue(uint8_t index, const char *fmt, ...)
{
    if (index >= mCount || !mVal[index])
        return;
    char buf[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(mVal[index], buf);
}

void DiagnosticsApp::onCreate(lv_obj_t *parent)
{
    if (mRoot)
        return;

    mRoot = lv_obj_create(parent);
    lv_obj_remove_style_all(mRoot);
    theme.styleScreen(mRoot);
    lv_obj_set_size(mRoot, metrics::screenW, metrics::contentH);
    lv_obj_set_pos(mRoot, 0, 0);
    lv_obj_remove_flag(mRoot, LV_OBJ_FLAG_SCROLLABLE);

    // Crash banner sits above the figures: if the device rebooted unexpectedly,
    // that is the first thing worth knowing.
    mCrash = lv_obj_create(mRoot);
    lv_obj_remove_style_all(mCrash);
    theme.styleCard(mCrash);
    lv_obj_set_size(mCrash, metrics::screenW - metrics::padL * 2, 26);
    lv_obj_set_pos(mCrash, metrics::padL, metrics::padS);
    lv_obj_set_style_bg_color(mCrash, lv_color_hex(theme.colors().error), 0);
    lv_obj_add_flag(mCrash, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(mCrash, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cl = lv_label_create(mCrash);
    lv_obj_set_style_text_font(cl, theme.fontSmall(), 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(theme.colors().text), 0);
    lv_obj_center(cl);
    lv_label_set_text(cl, "");

    // Two columns of figures; 12 lines will not fit in 200px otherwise.
    mList = lv_obj_create(mRoot);
    lv_obj_remove_style_all(mList);
    lv_obj_set_size(mList, metrics::screenW, metrics::contentH - 34);
    lv_obj_set_pos(mList, metrics::padL, 32);
    lv_obj_set_flex_flow(mList, LV_FLEX_FLOW_COLUMN_WRAP);
    lv_obj_set_style_pad_row(mList, 1, 0);
    lv_obj_set_style_pad_column(mList, metrics::padL, 0);
    lv_obj_remove_flag(mList, LV_OBJ_FLAG_SCROLLABLE);

    addLine(mList, "Uptime");
    addLine(mList, "Free heap");
    addLine(mList, "Heap low");
    addLine(mList, "Free PSRAM");
    addLine(mList, "CPU");
    addLine(mList, "Flash");
    addLine(mList, "Chat log");
    addLine(mList, "Frames");
    addLine(mList, "Frame ms");
    addLine(mList, "Flush us");
    addLine(mList, "Dropped");
    addLine(mList, "Mesh rx/tx");
    addLine(mList, "Neighbours");
    addLine(mList, "Air / pkt-min");

    lv_obj_t *hint = lv_label_create(mRoot);
    lv_obj_set_style_text_font(hint, theme.fontSmall(), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(theme.colors().textFaint), 0);
    lv_label_set_text(hint, "c  clear crash log");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -metrics::padL, -2);
}

void DiagnosticsApp::onShow(const AppArgs &args)
{
    (void)args;
    refresh();
}

bool DiagnosticsApp::onKey(uint32_t k)
{
    if (k == 'c' || k == 'C') {
        panic::clearCrashLog();
        shell.toast("Crash log cleared");
        refresh();
        return true;
    }
    return false;
}

void DiagnosticsApp::onTick()
{
    // Once a second is plenty; these are diagnostics, not a scope trace.
    const uint32_t now = millis();
    if (now - mLastRefreshMs < 1000)
        return;
    mLastRefreshMs = now;
    refresh();
}

void DiagnosticsApp::refresh()
{
    const uint32_t up = millis() / 1000;
    if (up >= 3600)
        setValue(kUptime, "%luh %lum", (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60));
    else
        setValue(kUptime, "%lum %lus", (unsigned long)(up / 60), (unsigned long)(up % 60));

    setValue(kHeap, "%lu K", (unsigned long)(ESP.getFreeHeap() / 1024));
    setValue(kHeapMin, "%lu K", (unsigned long)(ESP.getMinFreeHeap() / 1024));
    setValue(kPsram, "%lu K", (unsigned long)(ESP.getFreePsram() / 1024));
    setValue(kCpu, "%lu MHz", (unsigned long)getCpuFrequencyMhz());

    const size_t used = fsUsedBytes();
    const size_t total = fsTotalBytes();
    setValue(kFlash, "%lu/%lu K", (unsigned long)(used / 1024), (unsigned long)(total / 1024));
    setValue(kChat, "%lu K", (unsigned long)(chatStore.bytesUsed() / 1024));

    setValue(kFrames, "%lu", (unsigned long)shell.frameCount());
    setValue(kFrameMs, "%u", (unsigned)shell.lastFrameMs());
    setValue(kFlush, "%u", (unsigned)display.lastFlushUs());

    // Three independent drop counters, summarised. Any of them being nonzero
    // means something is falling behind and is worth investigating.
    const uint32_t drops = shell.droppedEvents() + keyboard.dropped() + service_.dropped();
    setValue(kDropped, "%lu", (unsigned long)drops);
    if (mVal[kDropped])
        lv_obj_set_style_text_color(mVal[kDropped],
                                    lv_color_hex(drops ? theme.colors().warn : theme.colors().text), 0);

    setValue(kMesh, "%lu/%lu", (unsigned long)mesh.messagesReceived(), (unsigned long)mesh.messagesSent());

    const auto den = mesh.density();
    // Two different questions: who is on the air right now (what drives the
    // bars) versus who NodeDB has ever catalogued.
    setValue(kNeighbours, "%u near / %u known", (unsigned)den.activeNeighbours, (unsigned)den.nodesRecent);
    if (den.quietSecs == 0xFFFF)
        setValue(kAirtime, "%u%% / %u/min / quiet", (unsigned)den.utilizationPct, (unsigned)den.packetsPerMin);
    else
        setValue(kAirtime, "%u%% / %u/min / %us ago", (unsigned)den.utilizationPct, (unsigned)den.packetsPerMin,
                 (unsigned)den.quietSecs);

    // Crash banner.
    if (panic::hadCrash()) {
        lv_obj_remove_flag(mCrash, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *lbl = lv_obj_get_child(mCrash, 0);
        if (lbl)
            lv_label_set_text(lbl, panic::lastCrashSummary());
    } else {
        lv_obj_add_flag(mCrash, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace pgros

#endif // PGROS
