#ifdef PGROS

//
// On-disk format ("PGTK", PgrOS track, version 1)
//
//   offset  size  field
//        0     4  magic 'P','G','T','K'
//        4     1  version
//        5     3  reserved
//   then a bare sequence of 24-byte records:
//        0     4  time      epoch seconds, little-endian
//        4     4  latI      degrees * 1e7
//        8     4  lonI
//       12     4  altM      metres
//       16     1  sats
//       17     1  bars
//       18     1  directNeighbours
//       19     1  packetsPerMin
//       20     1  bestRssi  (int8)
//       21     1  bestSnr   (int8)
//       22     1  flags
//       23     1  reserved
//
// Fixed-size records mean a power cut costs at most the record in flight, and a
// torn tail shows up as a file length that is not a whole number of records --
// checked and truncated on open. No per-record CRC: losing one breadcrumb is not
// worth the bytes, and the ranges are validated on read anyway.
//

#include "store/TrackStore.h"

#include "configuration.h"

#include "FSCommon.h"
#include "SPILock.h"
#include "concurrency/LockGuard.h"
#include "core/Policy.h"

#if defined(HAS_SDCARD)
#include <SD.h>
#endif

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace pgros
{

TrackStore trackStore;

static const char *kDir = "/pgros/track";
static const char *kPath = "/pgros/track/track.pgtk";
static bool sUseSd = false;

static constexpr uint8_t kVersion = 1;
static constexpr size_t kHeaderBytes = 8;

static fs::FS &trackFs()
{
#if defined(HAS_SDCARD)
    if (sUseSd)
        return SD;
#endif
    return FSCom;
}

// ---------------------------------------------------------------------------

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void encode(const TrackPoint &pt, uint8_t *b)
{
    put32(b + 0, pt.time);
    put32(b + 4, (uint32_t)pt.latI);
    put32(b + 8, (uint32_t)pt.lonI);
    put32(b + 12, (uint32_t)pt.altM);
    b[16] = pt.sats;
    b[17] = pt.bars;
    b[18] = pt.directNeighbours;
    b[19] = pt.packetsPerMin;
    b[20] = (uint8_t)pt.bestRssi;
    b[21] = (uint8_t)pt.bestSnr;
    b[22] = pt.flags;
    b[23] = 0;
}

static void decode(const uint8_t *b, TrackPoint &pt)
{
    pt.time = get32(b + 0);
    pt.latI = (int32_t)get32(b + 4);
    pt.lonI = (int32_t)get32(b + 8);
    pt.altM = (int32_t)get32(b + 12);
    pt.sats = b[16];
    pt.bars = b[17];
    pt.directNeighbours = b[18];
    pt.packetsPerMin = b[19];
    pt.bestRssi = (int8_t)b[20];
    pt.bestSnr = (int8_t)b[21];
    pt.flags = b[22];
    pt.reserved = 0;
}

// Equirectangular approximation. Points are seconds apart, so the error against
// a proper haversine is far below GPS noise and it costs one cos().
static uint32_t metresBetween(int32_t aLatI, int32_t aLonI, int32_t bLatI, int32_t bLonI)
{
    const float aLat = aLatI / 1e7f;
    const float dLat = (bLatI - aLatI) / 1e7f;
    const float dLon = (bLonI - aLonI) / 1e7f;
    const float x = dLon * cosf(aLat * 3.14159265f / 180.0f);
    return (uint32_t)(sqrtf(dLat * dLat + x * x) * 111320.0f);
}

// ---------------------------------------------------------------------------

bool TrackStore::begin()
{
    sUseSd = false;
#if defined(HAS_SDCARD)
    {
        concurrency::LockGuard g(spiLock);
        if (SD.cardType() != CARD_NONE) {
            if (!SD.exists(kDir))
                SD.mkdir(kDir);
            sUseSd = true;
        }
    }
#endif
    if (!sUseSd) {
        concurrency::LockGuard g(spiLock);
        if (!FSCom.exists("/pgros"))
            FSCom.mkdir("/pgros");
        if (!FSCom.exists(kDir))
            FSCom.mkdir(kDir);
    }

    // Repair a torn tail: anything that is not a whole number of records after
    // the header is a half-written point from a power cut.
    concurrency::LockGuard g(spiLock);
    File f = trackFs().open(kPath, FILE_O_READ);
    if (f) {
        const size_t sz = f.size();
        f.close();
        if (sz > kHeaderBytes) {
            const size_t body = sz - kHeaderBytes;
            const size_t ragged = body % kRecordBytes;
            if (ragged)
                LOG_WARN("TrackStore: %u ragged bytes at EOF, ignoring", (unsigned)ragged);
        }
    }

    mReady = true;
    LOG_INFO("TrackStore: ready (%s)", sUseSd ? "SD card" : "internal flash");
    return true;
}

bool TrackStore::recording() const
{
    return mReady && policy.get().storeGpsTrack;
}

bool TrackStore::offer(const TrackPoint &in)
{
    if (!recording())
        return false;
    if (!in.latI && !in.lonI)
        return false; // a zeroed fix is "no fix", not the Gulf of Guinea

    TrackPoint pt = in;

    // Rate limit. Standing still must not fill the card, but a point every so
    // often is still wanted so a stationary period is visible as dwell time.
    if (mHavePrev) {
        const uint32_t moved = metresBetween(mPrev.latI, mPrev.lonI, pt.latI, pt.lonI);
        const uint32_t elapsed = (pt.time > mPrev.time) ? (pt.time - mPrev.time) : 0;

        if (moved < kMinMetres && elapsed < kMinSecs)
            return false;

        if (elapsed > kSegmentGapSecs)
            pt.flags |= kTrackSegmentStart;
    } else {
        pt.flags |= kTrackSegmentStart;
    }

    concurrency::LockGuard g(spiLock);

    // Guard the internal-flash ceiling. The card gets no such limit.
    if (!sUseSd) {
        File probe = trackFs().open(kPath, FILE_O_READ);
        const size_t sz = probe ? probe.size() : 0;
        if (probe)
            probe.close();
        if (sz >= kMaxFlashBytes) {
            LOG_WARN("TrackStore: internal log full (%u bytes); recording paused", (unsigned)sz);
            return false;
        }
    }

    const bool fresh = !trackFs().exists(kPath);
    File f = trackFs().open(kPath, fresh ? FILE_O_WRITE : "a");
    if (!f) {
        LOG_ERROR("TrackStore: cannot open %s", kPath);
        return false;
    }

    if (fresh) {
        uint8_t hdr[kHeaderBytes] = {'P', 'G', 'T', 'K', kVersion, 0, 0, 0};
        f.write(hdr, sizeof(hdr));
    }

    uint8_t rec[kRecordBytes];
    encode(pt, rec);
    const size_t put = f.write(rec, sizeof(rec));
    f.close();

    if (put != sizeof(rec)) {
        LOG_ERROR("TrackStore: short write");
        return false;
    }

    mPrev = pt;
    mHavePrev = true;
    return true;
}

bool TrackStore::stats(TrackStats &out)
{
    out = TrackStats();
    if (!mReady)
        return false;

    concurrency::LockGuard g(spiLock);
    File f = trackFs().open(kPath, FILE_O_READ);
    if (!f)
        return false;

    out.bytes = f.size();
    if (out.bytes <= kHeaderBytes) {
        f.close();
        return true;
    }
    f.seek(kHeaderBytes);

    uint8_t rec[kRecordBytes];
    TrackPoint pt, prev;
    bool havePrev = false;

    while (f.read(rec, sizeof(rec)) == sizeof(rec)) {
        decode(rec, pt);
        out.points++;
        if (!out.firstTime)
            out.firstTime = pt.time;
        out.lastTime = pt.time;
        if (pt.bars < out.worstBars)
            out.worstBars = pt.bars;
        if (havePrev && !(pt.flags & kTrackSegmentStart))
            out.metres += metresBetween(prev.latI, prev.lonI, pt.latI, pt.lonI);
        prev = pt;
        havePrev = true;
    }
    f.close();
    return true;
}

// ---------------------------------------------------------------------------
// GPX
// ---------------------------------------------------------------------------

bool TrackStore::exportGpx(EmitFn emit, void *ctx)
{
    if (!mReady || !emit)
        return false;

    concurrency::LockGuard g(spiLock);
    File f = trackFs().open(kPath, FILE_O_READ);
    if (!f)
        return false;

    char buf[512];

    // The pgros namespace is declared so the extensions are well-formed. Readers
    // that do not know it are required to ignore it, which is the point.
    int n = snprintf(buf, sizeof(buf),
                     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<gpx version=\"1.1\" creator=\"PgrOS\"\n"
                     "     xmlns=\"http://www.topografix.com/GPX/1/1\"\n"
                     "     xmlns:pgros=\"https://github.com/Archaejohn/PgrOS/gpx/1\">\n"
                     "<trk><name>PgrOS track</name>\n");
    if (!emit(ctx, buf, (size_t)n)) {
        f.close();
        return false;
    }

    if (f.size() > kHeaderBytes)
        f.seek(kHeaderBytes);

    uint8_t rec[kRecordBytes];
    TrackPoint pt;
    bool segOpen = false;
    bool ok = true;

    while (ok && f.read(rec, sizeof(rec)) == sizeof(rec)) {
        decode(rec, pt);

        // Reject anything outside the possible range rather than emitting XML
        // that a parser will choke on.
        if (pt.latI > 900000000 || pt.latI < -900000000 || pt.lonI > 1800000000 || pt.lonI < -1800000000)
            continue;

        if ((pt.flags & kTrackSegmentStart) || !segOpen) {
            if (segOpen)
                ok = emit(ctx, "</trkseg>\n", 10);
            if (ok)
                ok = emit(ctx, "<trkseg>\n", 9);
            segOpen = true;
            if (!ok)
                break;
        }

        // ISO 8601 UTC. A zero timestamp means the clock was never set, so the
        // element is omitted rather than claiming 1970.
        char timeEl[48] = {0};
        if (pt.time) {
            time_t t = (time_t)pt.time;
            struct tm tmv;
            gmtime_r(&t, &tmv);
            strftime(timeEl, sizeof(timeEl), "<time>%Y-%m-%dT%H:%M:%SZ</time>", &tmv);
        }

        char sig[40];
        if (pt.flags & kTrackHeardDirect)
            snprintf(sig, sizeof(sig), "%ddBm %ddB", (int)pt.bestRssi, (int)pt.bestSnr);
        else
            snprintf(sig, sizeof(sig), "no direct");

        n = snprintf(
            buf, sizeof(buf),
            "<trkpt lat=\"%.7f\" lon=\"%.7f\"><ele>%ld</ele>%s"
            "<sat>%u</sat>"
            // Also in <desc>, because most viewers silently drop extensions they
            // do not recognise and this is the whole point of the file.
            "<desc>mesh %u/4, %u direct, %s, %u pkt/min</desc>"
            "<extensions><pgros:mesh"
            " bars=\"%u\" neighbours=\"%u\" packetsPerMin=\"%u\""
            " rssi=\"%d\" snr=\"%d\" direct=\"%u\"/></extensions>"
            "</trkpt>\n",
            pt.latI / 1e7, pt.lonI / 1e7, (long)pt.altM, timeEl, (unsigned)pt.sats, (unsigned)pt.bars,
            (unsigned)pt.directNeighbours, sig, (unsigned)pt.packetsPerMin, (unsigned)pt.bars,
            (unsigned)pt.directNeighbours, (unsigned)pt.packetsPerMin, (int)pt.bestRssi, (int)pt.bestSnr,
            (pt.flags & kTrackHeardDirect) ? 1u : 0u);

        if (n > 0)
            ok = emit(ctx, buf, (size_t)n);
    }

    if (ok && segOpen)
        ok = emit(ctx, "</trkseg>\n", 10);
    if (ok)
        ok = emit(ctx, "</trk>\n</gpx>\n", 14);

    f.close();
    return ok;
}

bool TrackStore::erase()
{
    concurrency::LockGuard g(spiLock);
    mHavePrev = false;
    return trackFs().remove(kPath);
}

uint32_t TrackStore::bytesUsed()
{
    concurrency::LockGuard g(spiLock);
    File f = trackFs().open(kPath, FILE_O_READ);
    if (!f)
        return 0;
    const uint32_t sz = f.size();
    f.close();
    return sz;
}

} // namespace pgros

#endif // PGROS
