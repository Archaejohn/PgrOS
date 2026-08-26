//
// PgrOS crash capture -- implementation.
//
// The design rationale, and specifically what this deliberately does NOT try to
// do, is in Panic.h. The short version: nothing here runs inside a panic
// context. Everything is reconstructed at the next boot from state that survives
// a reset, because that is the only version of this that works.
//
// ============================================================================
// /pgros/crash.log
// ============================================================================
//
// Plain text, one entry per line, newest last, at most kMaxEntries lines. It is
// meant to be legible on a 480x222 panel and in a serial dump, so it is not a
// binary format. An entry looks like:
//
//   reset=TASK_WDT boots=2 up>=41200ms heap=214532 fw=2.7.11.abcdef
//
// "up>=" is a lower bound, not the uptime at the fault -- see checkpoint().
//
// The file is rewritten whole on every append (read the tail, drop the oldest
// entry, write it back through SafeFile). At four short lines that is cheaper
// and far safer than trying to trim a file in place on LittleFS.
//

#ifdef PGROS

#include "Panic.h"

#include "FSCommon.h"
#include "SafeFile.h"
#include "configuration.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef ARCH_ESP32
#include <esp_attr.h>
#include <esp_system.h>

// The 16MB partition table reserves a 64KB `coredump` partition, but a build
// only populates it if the IDF was configured with coredump-to-flash. The stock
// Arduino-ESP32 prebuilt libraries are built with CONFIG_ESP_COREDUMP_ENABLE_TO_
// NONE, so on an unmodified build this block compiles out and we fall back to
// the reset reason alone. If you turn coredump on in custom_sdkconfig, the
// faulting task and PC start appearing in the log for free.
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
#if __has_include(<esp_core_dump.h>)
#include <esp_core_dump.h>
#define PGROS_HAVE_COREDUMP 1
#endif
#endif
#endif // ARCH_ESP32

namespace pgros
{
namespace panic
{

namespace
{

constexpr const char *kCrashDir = "/pgros";
constexpr const char *kCrashLogPath = "/pgros/crash.log";

constexpr size_t kMaxEntries = 4;
constexpr size_t kMaxLine = 160; // including the trailing newline

// ---------------------------------------------------------------------------
// State that has to survive a reset
// ---------------------------------------------------------------------------
//
// RTC-noinit memory is retained across a software, panic or watchdog reset and
// is NOT zeroed by the startup code -- which is the point, but it also means it
// is arbitrary garbage after a power-on reset. Hence the magic word: if it does
// not match we treat the block as empty, which is exactly right, because
// unplugging the device should reset the boot-loop count.
//
// This is not NVS. It is deliberately not NVS: an NVS write on every boot costs
// a flash erase cycle on the boot path, and a boot-loop counter that survives a
// power cycle would make "unplug it and try again" look like a boot loop.

#ifdef ARCH_ESP32
#define PGROS_RTC_NOINIT RTC_NOINIT_ATTR
#else
#define PGROS_RTC_NOINIT
#endif

constexpr uint32_t kRtcMagic = 0x50475231; // 'PGR1'

PGROS_RTC_NOINIT uint32_t sRtcMagic;
PGROS_RTC_NOINIT uint32_t sBootAttempts;   // boots since the last noteBootOk()
PGROS_RTC_NOINIT uint32_t sLastCheckpoint; // millis() at the last checkpoint()
PGROS_RTC_NOINIT uint32_t sOrderlyShutdown; // 1 if esp_restart() was on its way

// ---------------------------------------------------------------------------
// In-RAM state
// ---------------------------------------------------------------------------

char sSummary[kMaxLine] = {0};
bool sBegun = false;
bool sCrashDetected = false;
uint8_t sBootAttemptsAtBoot = 0;

const char *resetReasonName(int reason)
{
#ifdef ARCH_ESP32
    switch (reason) {
    case ESP_RST_POWERON:
        return "POWERON";
    case ESP_RST_EXT:
        return "EXT_PIN";
    case ESP_RST_SW:
        return "SW_RESTART";
    case ESP_RST_PANIC:
        return "PANIC";
    case ESP_RST_INT_WDT:
        return "INT_WDT";
    case ESP_RST_TASK_WDT:
        return "TASK_WDT";
    case ESP_RST_WDT:
        return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP:
        return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
        return "BROWNOUT";
    case ESP_RST_SDIO:
        return "SDIO";
    default:
        return "UNKNOWN";
    }
#else
    (void)reason;
    return "UNKNOWN";
#endif
}

/// Which reset reasons mean "something went wrong", as opposed to a deliberate
/// restart or a normal power-up. Brownout is included: on a battery device it is
/// usually a real fault (a LoRa TX burst on a tired cell) and the user wants to
/// know.
bool reasonIsFault(int reason)
{
#ifdef ARCH_ESP32
    switch (reason) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
        return true;
    default:
        return false;
    }
#else
    (void)reason;
    return false;
#endif
}

/// snprintf returns what it WOULD have written, so naively accumulating its
/// return value walks `n` past the end of the buffer and makes the next
/// `sizeof(buf) - n` underflow into a huge size_t. This clamps instead.
void appendFmt(char *buf, size_t bufSize, size_t &n, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

void appendFmt(char *buf, size_t bufSize, size_t &n, const char *fmt, ...)
{
    if (n + 1 >= bufSize)
        return;

    va_list ap;
    va_start(ap, fmt);
    const int wrote = vsnprintf(buf + n, bufSize - n, fmt, ap);
    va_end(ap);

    if (wrote < 0)
        return;
    n += (size_t)wrote;
    if (n >= bufSize)
        n = bufSize - 1;
}

/// Appends one line to the crash log, dropping the oldest entries so that at
/// most kMaxEntries remain. Rewrites the file whole via SafeFile.
bool appendEntry(const char *line)
{
    // Static rather than automatic: this runs on the setup() stack, which we do
    // not want to grow by 640 bytes, and it is only ever touched from begin().
    static char buf[kMaxEntries * kMaxLine];

    size_t used = 0;
    bool droppedHead = false;

    File f = FSCom.open(kCrashLogPath, FILE_O_READ);
    if (f) {
        const size_t fileSize = f.size();
        // Leave room for the line we are about to add.
        size_t want = sizeof(buf) - kMaxLine;
        if (fileSize > want) {
            f.seek(fileSize - want);
            droppedHead = true; // we are starting mid-line
        } else {
            want = fileSize;
        }
        const int got = f.read(reinterpret_cast<uint8_t *>(buf), want);
        used = (got > 0) ? (size_t)got : 0;
        f.close();
    }

    size_t start = 0;

    // If we seeked into the middle of the file the first line is a fragment.
    if (droppedHead) {
        while (start < used && buf[start] != '\n')
            start++;
        if (start < used)
            start++;
    }

    // Keep at most kMaxEntries-1 existing lines, so that adding ours lands on
    // exactly kMaxEntries.
    size_t newlines = 0;
    for (size_t i = start; i < used; i++)
        if (buf[i] == '\n')
            newlines++;

    while (newlines >= kMaxEntries && start < used) {
        while (start < used && buf[start] != '\n')
            start++;
        if (start < used) {
            start++;
            newlines--;
        }
    }

    FSCom.mkdir(kCrashDir);

    SafeFile out(kCrashLogPath, true);
    if (used > start)
        out.write(reinterpret_cast<const uint8_t *>(buf + start), used - start);
    out.write(reinterpret_cast<const uint8_t *>(line), strlen(line));
    out.write((uint8_t)'\n');

    if (!out.close()) {
        LOG_ERROR("PgrOS: could not write crash log");
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

bool begin()
{
    if (sBegun)
        return true;
    sBegun = true;

#ifdef ARCH_ESP32
    const int reason = (int)esp_reset_reason();
#else
    const int reason = 0;
#endif

    // Validate the RTC block. Garbage after a power-on reset, valid after any
    // reset that kept RTC memory alive.
    const bool rtcValid = (sRtcMagic == kRtcMagic);
    if (!rtcValid) {
        sRtcMagic = kRtcMagic;
        sBootAttempts = 0;
        sLastCheckpoint = 0;
        sOrderlyShutdown = 0;
    }

    const uint32_t lastUptime = rtcValid ? sLastCheckpoint : 0;
    const bool wasOrderly = rtcValid && sOrderlyShutdown == 1;

    sBootAttempts++;
    sBootAttemptsAtBoot = (sBootAttempts > 255) ? 255 : (uint8_t)sBootAttempts;

    // This boot's checkpoint starts at zero; the previous value has been read.
    sLastCheckpoint = 0;
    sOrderlyShutdown = 0;

#ifdef ARCH_ESP32
    // Runs on esp_restart(), i.e. an orderly reboot: a firmware update, a user
    // "reboot" command, a deliberate restart after a config change. It does NOT
    // run on a panic, an abort, a watchdog reset or a brownout, which is why it
    // cannot be used to capture the crash reason. All it does is let the next
    // boot tell "we meant to do that" apart from "we fell over".
    esp_register_shutdown_handler([]() { sOrderlyShutdown = 1; });
#endif

    const bool fault = reasonIsFault(reason);
    const bool loop = inBootLoop();
    sCrashDetected = fault || loop;

    // Build the summary line. Kept to one line and under kMaxLine so it can be
    // shown verbatim in Settings.
    size_t n = 0;
    appendFmt(sSummary, sizeof(sSummary), n, "reset=%s boots=%u", resetReasonName(reason), (unsigned)sBootAttemptsAtBoot);

    if (lastUptime)
        appendFmt(sSummary, sizeof(sSummary), n, " up>=%ums", (unsigned)lastUptime);

#ifdef ARCH_ESP32
    appendFmt(sSummary, sizeof(sSummary), n, " heap=%u", (unsigned)esp_get_free_heap_size());
#endif

    appendFmt(sSummary, sizeof(sSummary), n, " fw=%s", optstr(APP_VERSION));

#ifdef PGROS_HAVE_COREDUMP
    // Only reachable when the IDF was configured with coredump-to-flash. The
    // summary gives us the faulting task and the exception PC; the backtrace is
    // available too but is far too wide for the panel, so it stays out of the
    // one-line summary and only the top frame is included.
    if (fault && esp_core_dump_image_check() == ESP_OK) {
        esp_core_dump_summary_t summary;
        if (esp_core_dump_get_summary(&summary) == ESP_OK) {
            appendFmt(sSummary, sizeof(sSummary), n, " task=%s pc=0x%08x", summary.exc_task, (unsigned)summary.exc_pc);
        }
    }
#endif

    if (loop)
        appendFmt(sSummary, sizeof(sSummary), n, " BOOTLOOP");

    if (!sCrashDetected) {
        LOG_INFO("PgrOS: %s%s", sSummary, wasOrderly ? " (orderly)" : "");
        return true;
    }

    LOG_WARN("PgrOS: previous boot ended badly: %s", sSummary);
    return appendEntry(sSummary);
}

bool hadCrash()
{
    return sCrashDetected;
}

const char *lastCrashSummary()
{
    return sSummary;
}

void clearCrashLog()
{
    if (FSCom.exists(kCrashLogPath) && !FSCom.remove(kCrashLogPath))
        LOG_WARN("PgrOS: could not remove %s", kCrashLogPath);

    sSummary[0] = '\0';
    sCrashDetected = false;

    // Dismissing the banner also forgives the boot loop; otherwise the safe-mode
    // prompt would come back on the next boot even after a successful one.
    sBootAttempts = 0;
    sBootAttemptsAtBoot = 0;
}

void noteBootOk()
{
    checkpoint();

    if (sBootAttempts != 0) {
        LOG_INFO("PgrOS: boot reached the UI, clearing boot-attempt counter (was %u)", (unsigned)sBootAttempts);
        sBootAttempts = 0;
    }

    // sBootAttemptsAtBoot is deliberately NOT cleared: it is what this boot
    // looked like on the way in, and the crash banner still wants to say so.
}

void checkpoint()
{
    sRtcMagic = kRtcMagic;
    sLastCheckpoint = millis();
}

uint8_t bootAttempts()
{
    return sBootAttemptsAtBoot;
}

bool inBootLoop()
{
    return sBootAttemptsAtBoot > kBootLoopThreshold;
}

} // namespace panic
} // namespace pgros

#endif // PGROS
