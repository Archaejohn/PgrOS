#pragma once
//
// PgrOS crash capture.
//
// The requirement is that the device does not silently reboot: after a crash the
// user should be able to open Settings and see *something* about what happened,
// without a serial cable.
//
// ---------------------------------------------------------------------------
// WHAT THIS CAN AND CANNOT DO -- read this before extending it
// ---------------------------------------------------------------------------
//
// It is tempting to write the crash reason to LittleFS from inside the panic
// handler. That does not work reliably and we do not do it:
//
//   * A panic runs with interrupts disabled, on the faulting task's stack, with
//     the scheduler stopped. LittleFS on ESP32 takes a mutex and calls into the
//     flash driver, both of which need a working scheduler. The write either
//     hangs, faults again, or corrupts the filesystem.
//   * The heap may already be the thing that is broken. A crash caused by heap
//     corruption is exactly the crash you most want logged, and exactly the one
//     where allocating inside the handler is least safe.
//   * Writing to the same flash chip that is executing code, from an unwound
//     context, is a good way to turn a recoverable panic into a brick.
//
// So PgrOS captures crashes at the NEXT BOOT instead, which runs with a healthy
// scheduler and a healthy filesystem:
//
//   * esp_reset_reason() tells us *how* the last reset happened (panic, task
//     watchdog, interrupt watchdog, brownout, deliberate restart, power-on).
//     This is stored by the ROM/IDF and is entirely reliable.
//   * The ESP32 coredump partition, if the build has coredump-to-flash enabled,
//     adds the faulting task name, the exception PC and a short backtrace. The
//     16MB partition table does reserve a 64KB `coredump` partition, but the
//     stock Arduino-ESP32 prebuilt libraries ship with
//     CONFIG_ESP_COREDUMP_ENABLE_TO_NONE, so on an unmodified build this part is
//     compiled out and only the reset reason is available. See Panic.cpp.
//   * Free heap and firmware version at boot are recorded for context.
//
// What is therefore NOT available: the program counter and backtrace on a stock
// build, the value of any local variable, and the exact uptime at the moment of
// the fault. `checkpoint()` below narrows the uptime question but does not
// answer it exactly.
//
// ---------------------------------------------------------------------------
// Boot loops
// ---------------------------------------------------------------------------
//
// A counter in RTC-noinit memory is incremented by begin() and cleared by
// noteBootOk(). RTC memory survives a software/panic/watchdog reset but is
// garbage after a power-on reset, so it is validated against a magic word; an
// unplug-and-replug legitimately gives a clean slate. If the device resets
// repeatedly before the UI comes up the counter climbs, and hadCrash() starts
// reporting true so the shell can offer safe mode.
//

#include <stdint.h>

namespace pgros
{
namespace panic
{

/// Number of boots that must fail to reach noteBootOk() before we call it a
/// boot loop.
static constexpr uint8_t kBootLoopThreshold = 4;

// Reads the previous reset reason, updates the boot-attempt counter and appends
// a line to /pgros/crash.log if the last reset looks like a crash.
//
// MUST be called after fsInit() -- it writes to the filesystem. It is cheap
// (one small read, at most one small write) and safe to call exactly once.
//
// Returns false only if the crash log could not be written; the reset-reason and
// boot-loop information is still valid in that case.
bool begin();

// True if the previous reset looks like a fault (panic, abort, either watchdog,
// or brownout) OR if we are in a boot loop. This is the single predicate the UI
// uses to decide whether to show the crash banner and offer safe mode.
bool hadCrash();

// One line, NUL-terminated, safe to display. Points at a static buffer owned by
// this module; valid for the lifetime of the process. Returns an empty string if
// begin() has not run or there is nothing to report.
const char *lastCrashSummary();

// Deletes /pgros/crash.log and clears the in-memory summary and the boot-loop
// counter. Called when the user dismisses the crash banner.
void clearCrashLog();

// Called once the UI is up and the device is demonstrably usable. Clears the
// boot-attempt counter, which is what stops a boot loop being declared.
void noteBootOk();

// Optional. Stamps the current millis() into RTC memory so that, after an
// unexpected reset, begin() can report "the device had been up for at least N
// ms". Call it from a slow periodic task if you want that number to be useful;
// noteBootOk() calls it once already. Without periodic calls the reported uptime
// is simply the time to the last checkpoint, which is why it is described as a
// lower bound and never as "uptime at crash".
void checkpoint();

// How many boots in a row have failed to reach noteBootOk(), including this one.
uint8_t bootAttempts();

// True when bootAttempts() has passed kBootLoopThreshold.
bool inBootLoop();

} // namespace panic
} // namespace pgros
