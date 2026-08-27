#pragma once
//
// Time zone, guessed from GPS and overridable by hand.
//
// Meshtastic stores a POSIX TZ string in config.device.tzdef and applies it with
// setenv("TZ", ...) + tzset(). Out of the box on this device it is "GMT0", so
// every timestamp on screen is UTC -- which is why message times look wrong.
//
// ---------------------------------------------------------------------------
// What this can and cannot do
// ---------------------------------------------------------------------------
//
// Deriving a time zone from a coordinate properly needs the tz boundary
// database: megabytes of polygons. That is not going on a 3.375 MiB partition.
//
// What is here instead is a curated table of ~30 common zones, each with a real
// POSIX rule (so daylight saving is handled correctly) and a representative
// centroid. On a GPS fix the nearest centroid wins.
//
// That is a GUESS, and it is wrong in two predictable ways:
//
//   * Near a boundary. Nevada and Arizona are a degree apart in longitude and
//     an hour apart for half the year; a centroid cannot separate them.
//   * In countries that span several solar hours on one legal clock -- China is
//     the obvious case, India's half-hour offset another.
//
// So the guess is a starting point, not an authority. Setting the zone by hand
// turns the guessing off and it stays off. The phone app can also write tzdef,
// and that is still the most accurate source.

#include <stdint.h>

namespace pgros {

struct TimeZoneEntry {
    const char *label; // shown in Settings
    const char *posix; // POSIX TZ string, including DST rules where they apply
    int16_t lat;       // representative centroid, whole degrees
    int16_t lon;
};

namespace timezone_ {

// The curated table. Index 0 is UTC and is the fallback for everything.
extern const TimeZoneEntry kZones[];
extern const uint8_t kZoneCount;

// Applies the stored preference (auto or manual) at boot. Cheap; safe to call
// before the GPS has a fix.
void begin();

// Offer a new fix. Does nothing unless auto mode is on and the nearest zone has
// actually changed. Safe to call on every fix.
void onGpsFix(int32_t latI, int32_t lonI);

// Apply a zone by index. Turns auto mode off -- an explicit choice should not be
// silently overwritten the next time the GPS reports in.
void setManual(uint8_t index);

// Turn the GPS guess back on and re-evaluate at the next fix.
void setAuto();

bool isAuto();
uint8_t currentIndex();
const char *currentLabel();

// Nearest zone to a coordinate, by centroid. Exposed for the settings screen so
// it can show what auto mode would pick.
uint8_t nearest(int32_t latI, int32_t lonI);

} // namespace timezone_
} // namespace pgros
