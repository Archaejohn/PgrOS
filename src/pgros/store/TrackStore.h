#pragma once
//
// GPS track recording, annotated with mesh coverage.
//
// A plain breadcrumb trail is mildly interesting. A breadcrumb trail that also
// records how much mesh you could hear at each point is a coverage map -- it
// answers "where did I lose the mesh", which is the question worth asking on a
// device like this.
//
// So every point carries the usual lat/lon/elevation plus, at that instant:
//
//   directNeighbours  distinct nodes heard at zero hops
//   bars              the density figure the status bar shows
//   bestRssi/bestSnr  strongest DIRECT packet in the last minute
//   packetsPerMin     everything on the air, relayed traffic included
//
// Exported as GPX. The mesh figures ride in <extensions>, which is the standard
// place for them, and are ALSO written into <desc> so they are readable in
// viewers that ignore unknown extensions -- which is most of them.
//
// Records are fixed size and appended, so a power cut costs at most the record
// in flight; a torn tail is detected by file length and truncated on open.
//
// Storage follows the gallery: the SD card when one is present, internal flash
// otherwise. At the default cadence a point is ~28 bytes, so an hour of walking
// is about 10 KB -- nothing on a card, but capped on internal flash where it
// shares 3.375 MiB with the chat log and the Meshtastic config.

#include <stddef.h>
#include <stdint.h>

namespace pgros {

// One recorded point. Fixed size on disk; see kRecordBytes.
struct TrackPoint {
    uint32_t time;    // epoch seconds; 0 means the clock was not set
    int32_t latI;     // degrees * 1e7
    int32_t lonI;
    int32_t altM;     // metres
    uint8_t sats;
    uint8_t bars;             // 0..4 mesh density
    uint8_t directNeighbours;
    uint8_t packetsPerMin;    // capped at 255
    int8_t bestRssi;          // dBm, 0 when nothing was heard directly
    int8_t bestSnr;           // dB
    uint8_t flags;            // TrackFlags
    uint8_t reserved;
};

enum TrackFlags : uint8_t {
    kTrackNone = 0,
    kTrackHeardDirect = 1 << 0, // bestRssi/bestSnr are meaningful
    kTrackSegmentStart = 1 << 1, // first point after a gap; starts a new <trkseg>
};

struct TrackStats {
    uint32_t points = 0;
    uint32_t bytes = 0;
    uint32_t firstTime = 0;
    uint32_t lastTime = 0;
    uint32_t metres = 0;   // rough distance along the track
    uint8_t worstBars = 4; // lowest density seen
};

class TrackStore
{
  public:
    // Picks storage and creates the directory. Cheap; safe on the boot path.
    bool begin();
    bool ready() const { return mReady; }

    // Offer a fix. Ignores it unless recording is enabled and the point is far
    // enough or old enough to be worth keeping -- standing still must not fill
    // the card. Returns true if a point was written.
    bool offer(const TrackPoint &p);

    // Recording is driven by policy.storeGpsTrack; this reports the live state.
    bool recording() const;

    bool stats(TrackStats &out);

    // Streams the whole log as GPX. `emit` is called with chunks of text; it
    // returns false to abort. Kept as a callback so the portal can stream
    // straight to a socket without buffering a whole track in RAM.
    typedef bool (*EmitFn)(void *ctx, const char *data, size_t len);
    bool exportGpx(EmitFn emit, void *ctx);

    // Streams the track as compact JSON for the portal map. Points are arrays,
    // not objects, because the field names would otherwise be most of the
    // payload. Decimated to at most maxPoints so a long track does not become a
    // multi-megabyte response served off a microcontroller.
    bool exportJson(EmitFn emit, void *ctx, uint32_t maxPoints);

    // Enough to draw a useful line without flattening the interesting parts.
    static constexpr uint32_t kJsonMaxPoints = 1500;

    bool erase();
    uint32_t bytesUsed();

    // A new <trkseg> starts when this much time passes with no point, so a track
    // resumed the next day is not drawn as one straight line across the county.
    static constexpr uint32_t kSegmentGapSecs = 300;

    // Sampling. A point every kMinSecs at rest, or sooner once moved kMinMetres.
    static constexpr uint32_t kMinSecs = 20;
    static constexpr uint32_t kMinMetres = 15;

    // Ceiling when the log lives on internal flash.
    static constexpr uint32_t kMaxFlashBytes = 256 * 1024;

    static constexpr size_t kRecordBytes = 24;

  private:
    bool mReady = false;
    bool mHavePrev = false;
    TrackPoint mPrev = {};
};

extern TrackStore trackStore;

} // namespace pgros
