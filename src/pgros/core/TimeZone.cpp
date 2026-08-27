#ifdef PGROS

#include "core/TimeZone.h"

#include "configuration.h"

#include "NodeDB.h"
#include "core/EventBus.h"
#include "core/Policy.h"
#include "mesh/MeshService.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace pgros
{
namespace timezone_
{

// POSIX TZ strings, not IANA names: the C library takes the former directly and
// carries the DST rule with it, so no rule database is needed at runtime.
//
// Centroids are whole degrees and deliberately rough -- they only have to be
// closer to their own zone than to a neighbour's, and where that fails the user
// sets it by hand.
const TimeZoneEntry kZones[] = {
    {"UTC", "UTC0", 0, 0},

    // North America
    {"US Pacific", "PST8PDT,M3.2.0,M11.1.0", 38, -121},
    {"US Mountain", "MST7MDT,M3.2.0,M11.1.0", 40, -110},
    {"US Arizona", "MST7", 34, -112},
    {"US Central", "CST6CDT,M3.2.0,M11.1.0", 38, -95},
    {"US Eastern", "EST5EDT,M3.2.0,M11.1.0", 40, -77},
    {"Alaska", "AKST9AKDT,M3.2.0,M11.1.0", 64, -150},
    {"Hawaii", "HST10", 21, -157},
    {"Atlantic Canada", "AST4ADT,M3.2.0,M11.1.0", 45, -63},
    {"Newfoundland", "NST3:30NDT,M3.2.0,M11.1.0", 48, -56},
    {"Mexico City", "CST6", 19, -99},

    // South America
    {"Brazil East", "<-03>3", -23, -46},
    {"Argentina", "<-03>3", -34, -58},
    {"Chile", "<-04>4<-03>,M9.1.6/24,M4.1.6/24", -33, -71},
    {"Colombia", "<-05>5", 4, -74},

    // Europe / Africa
    {"UK & Ireland", "GMT0BST,M3.5.0/1,M10.5.0", 54, -2},
    {"Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3", 50, 10},
    {"Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4", 45, 25},
    {"Moscow", "MSK-3", 55, 37},
    {"South Africa", "SAST-2", -29, 25},
    {"West Africa", "WAT-1", 6, 3},
    {"East Africa", "EAT-3", -1, 37},

    // Asia
    {"UAE", "<+04>-4", 24, 54},
    {"Pakistan", "PKT-5", 30, 70},
    {"India", "IST-5:30", 22, 79},
    {"Thailand", "<+07>-7", 15, 101},
    {"China", "CST-8", 35, 105},
    {"Singapore", "<+08>-8", 1, 104},
    {"Japan", "JST-9", 36, 138},
    {"Korea", "KST-9", 37, 127},

    // Oceania
    {"Australia West", "AWST-8", -30, 120},
    {"Australia Central", "ACST-9:30ACDT,M10.1.0,M4.1.0/3", -25, 133},
    {"Australia East", "AEST-10AEDT,M10.1.0,M4.1.0/3", -33, 151},
    {"New Zealand", "NZST-12NZDT,M9.5.0,M4.1.0/3", -41, 174},
};

const uint8_t kZoneCount = (uint8_t)(sizeof(kZones) / sizeof(kZones[0]));

static uint8_t sCurrent = 0;
static bool sApplied = false;

// ---------------------------------------------------------------------------

static void apply(uint8_t index, bool persist)
{
    if (index >= kZoneCount)
        index = 0;

    const TimeZoneEntry &z = kZones[index];

    setenv("TZ", z.posix, 1);
    tzset();

    sCurrent = index;
    sApplied = true;

    // Mirror it into Meshtastic's own config so the phone app agrees with the
    // screen, and so the setting survives independently of PgrOS policy.
    if (strcmp(config.device.tzdef, z.posix) != 0) {
        strncpy(config.device.tzdef, z.posix, sizeof(config.device.tzdef) - 1);
        config.device.tzdef[sizeof(config.device.tzdef) - 1] = '\0';
        if (persist && service)
            service->reloadConfig(SEGMENT_CONFIG);
    }

    LOG_INFO("PgrOS: timezone %s (%s)%s", z.label, z.posix, isAuto() ? " [auto]" : "");
}

uint8_t nearest(int32_t latI, int32_t lonI)
{
    const float lat = latI / 1e7f;
    const float lon = lonI / 1e7f;

    // Longitude degrees converge towards the poles, so scale them by cos(lat)
    // before comparing. Without it a high-latitude fix drifts east or west.
    const float latScale = cosf(lat * 3.14159265f / 180.0f);

    uint8_t best = 0;
    float bestDist = 1e12f;

    // Skip index 0 (UTC): it has no real centroid and would win for anyone in
    // the Gulf of Guinea.
    for (uint8_t i = 1; i < kZoneCount; ++i) {
        float dLat = lat - (float)kZones[i].lat;
        float dLon = lon - (float)kZones[i].lon;

        // Shortest way round the globe, so a fix at 179E is near a zone at 179W.
        if (dLon > 180.0f)
            dLon -= 360.0f;
        else if (dLon < -180.0f)
            dLon += 360.0f;

        dLon *= latScale;

        const float dist = dLat * dLat + dLon * dLon;
        if (dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return best;
}

void begin()
{
    const Policy &p = policy.get();

    if (!p.tzAuto && p.tzIndex < kZoneCount) {
        apply(p.tzIndex, false);
        return;
    }

    // Auto, but no fix yet. Keep whatever Meshtastic already had -- often set by
    // the phone app, which is more accurate than anything guessed here -- rather
    // than stamping UTC over it.
    if (config.device.tzdef[0]) {
        setenv("TZ", config.device.tzdef, 1);
        tzset();
        LOG_INFO("PgrOS: timezone from config (%s), awaiting fix", config.device.tzdef);
    }
}

void onGpsFix(int32_t latI, int32_t lonI)
{
    if (!policy.get().tzAuto)
        return;
    if (!latI && !lonI)
        return; // a zeroed position is "no fix", not the Gulf of Guinea

    const uint8_t guess = nearest(latI, lonI);
    if (sApplied && guess == sCurrent)
        return;

    apply(guess, true);
    policy.get().tzIndex = guess;
    policy.markDirty();

    char note[48];
    snprintf(note, sizeof(note), "Time zone: %s", kZones[guess].label);
    postNotification(note, 0);
}

void setManual(uint8_t index)
{
    if (index >= kZoneCount)
        return;
    policy.get().tzAuto = false;
    policy.get().tzIndex = index;
    policy.markDirty();
    apply(index, true);
}

void setAuto()
{
    policy.get().tzAuto = true;
    policy.markDirty();
    sApplied = false; // force a re-evaluation on the next fix
}

bool isAuto()
{
    return policy.get().tzAuto;
}

uint8_t currentIndex()
{
    return sCurrent;
}

const char *currentLabel()
{
    return kZones[sCurrent < kZoneCount ? sCurrent : 0].label;
}

} // namespace timezone_
} // namespace pgros

#endif // PGROS
