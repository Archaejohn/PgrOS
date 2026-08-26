//
// PgrOS persistent chat storage -- on-disk implementation.
//
// ============================================================================
// ON-DISK FORMAT  ("PGCL", PgrOS chat log, version 1)
// ============================================================================
//
// One file per thread:
//     /pgros/ch/<channel>.log        broadcast, <channel> in decimal
//     /pgros/dm/<nodenum>.log        direct, <nodenum> as 8 lowercase hex digits
//
// A file is a bare concatenation of records. There is no file header: the first
// record starts at offset 0, and a zero-length file is a valid empty thread.
// Nothing already written is ever rewritten. Status changes and read marks are
// appended as their own records and folded in when the log is read.
//
// Every record is framed identically. All integers are little-endian and are
// written field by field into a byte buffer, so no struct packing or alignment
// assumption is involved anywhere.
//
//   off  size  field
//   ---  ----  --------------------------------------------------------------
//     0     2  magic       'P','G' (0x50 0x47)
//     2     1  version     kFormatVersion, currently 1
//     3     1  type        1 = message, 2 = status patch, 3 = read marker
//     4     2  recLen      total record length, header+payload+trailer
//     6     N  payload     type-specific, see below
//   L-6     2  crc16       CRC-16/CCITT-FALSE over bytes [0, L-6)
//   L-4     4  recLenDup   total record length again, as uint32
//
// L is recLen. Header is 6 bytes, trailer is 6, so payload is recLen-12.
//
// recLen appears twice on purpose. The trailing copy is what lets a reader seek
// to EOF, read the last 4 bytes, and jump straight to the start of the final
// record -- and then repeat, walking the file backwards without ever touching
// the bytes in front of it. The header copy is what lets the recovery scan walk
// forwards. The two must agree or the record is rejected.
//
// The CRC covers the header and the payload but not the trailer, because the
// trailer is validated by comparing it against the CRC-protected header copy.
//
// ---------------------------------------------------------------------------
// Payload, type 1 (message) -- 33 fixed bytes then three variable strings
// ---------------------------------------------------------------------------
//   off  size  field
//     0     4  packetId       Meshtastic packet id, 0 if none assigned
//     4     4  from           sender node number
//     8     4  to             destination node number, or broadcast
//    12     4  rxTime         epoch seconds, 0 if the clock was not set
//    16     4  uptimeMs       local millis() at insert
//    20     4  snr            IEEE-754 binary32
//    24     1  channel
//    25     1  hopsAway
//    26     1  rssi           int8
//    27     1  status         MsgStatus
//    28     1  flags          MsgFlags bitfield
//    29     1  shortLen       bytes of senderShort, <= 4
//    30     1  longLen        bytes of senderLong, <= 39
//    31     2  textLen        bytes of text, <= 237
//    33     shortLen  senderShort, no NUL
//     +     longLen   senderLong, no NUL
//     +     textLen   text, no NUL
//
// Strings are stored with an explicit length and no terminator, so a five
// character message costs 5 bytes and not 237.
//
// ---------------------------------------------------------------------------
// Payload, type 2 (status patch) -- 5 bytes
// ---------------------------------------------------------------------------
//     0     4  packetId       message this patches
//     4     1  status         MsgStatus to move it to
//
// ---------------------------------------------------------------------------
// Payload, type 3 (read marker) -- 8 bytes
// ---------------------------------------------------------------------------
//     0     4  rxTime         epoch seconds when marked, 0 if unknown
//     4     4  uptimeMs       local millis() when marked
//
// The marker's meaning is positional: every message earlier in the file than a
// read marker is read. The timestamps are informational.
//
// ============================================================================
// READING
// ============================================================================
//
// Backward walk. pos starts at the file size. Read the uint32 at pos-4, that is
// recLen; the record occupies [pos-recLen, pos). Validate, decode, then set
// pos -= recLen and repeat. Stop at pos == 0, at `max` messages, or at the
// first record that fails validation.
//
// Folding. Walking backwards means every patch is seen before the message it
// patches. Patches are accumulated in a small fixed table keyed by packetId and
// applied when the matching message record turns up:
//
//   * a status patch is applied only if it moves the message forward through
//     rank(): Unknown < Composing < Queued < Sent < Failed < Delivered
//     < Received. Delivered outranks Failed deliberately -- a late ack after a
//     retransmit timeout is good news. Received is highest so a stray patch can
//     never rewrite an inbound message.
//   * once a read marker has been passed, kFlagUnread is cleared on every
//     message decoded after it (i.e. every older message).
//
// ============================================================================
// RECOVERY
// ============================================================================
//
// Because the store only ever appends, the only byte range that can be damaged
// by a power loss is the tail. So the check on first access is cheap: validate
// the single record ending at EOF. If it parses, the file is accepted.
//
// If it does not, the file is scanned forward from offset 0, following each
// record's header recLen, until a record fails validation. Everything from that
// point to EOF is discarded and the good prefix is written to <path>.log.new
// and renamed over the original -- never truncated in place, since there is no
// truncate() in the Arduino FS API and a rename is atomic in LittleFS anyway.
// The number of bytes dropped is reported through StoreStats.
//
// A torn tail therefore costs one record. Bit rot in the middle of a file costs
// everything after it, which is the price of not being able to resynchronise on
// a frame boundary without a much larger magic.
//

#include "pgros/store/ChatStore.h"

#include "FSCommon.h"
#include "SPILock.h"
#include "configuration.h"
#include "gps/RTC.h"
#include "mesh/Channels.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>

// FSCommon.h defines FILE_O_READ/FILE_O_WRITE but nothing for append.
#ifndef FILE_O_APPEND
#define FILE_O_APPEND "a"
#endif

namespace pgros
{

namespace
{

// ---------------------------------------------------------------------------
// Format constants
// ---------------------------------------------------------------------------

constexpr uint8_t kMagic0 = 'P';
constexpr uint8_t kMagic1 = 'G';
constexpr uint8_t kFormatVersion = 1;

constexpr uint8_t kRecMessage = 1;
constexpr uint8_t kRecStatus = 2;
constexpr uint8_t kRecRead = 3;

constexpr size_t kHeaderLen = 6;                        // magic, version, type, recLen
constexpr size_t kTrailerLen = 6;                       // crc16, recLen duplicate
constexpr size_t kFrameLen = kHeaderLen + kTrailerLen;  // 12
constexpr size_t kMsgFixedLen = 33;
constexpr size_t kStatusPayloadLen = 5;
constexpr size_t kReadPayloadLen = 8;

// Longest a message record can legally be, and the smallest any record can be.
// Both are hard bounds: anything read off disk outside them is corruption.
constexpr size_t kMaxRecordLen = kFrameLen + kMsgFixedLen + kMaxShortName + kMaxLongName + kMaxTextLen;
constexpr size_t kMinRecordLen = kFrameLen + kStatusPayloadLen;

// Stack buffer for one serialised record. Records never heap-allocate.
constexpr size_t kRecordBufLen = 384;
static_assert(kRecordBufLen >= kMaxRecordLen, "record buffer too small for the largest message");

constexpr size_t kPathLen = 64;

// How far back summarise()/unreadCount() are willing to walk. A thread whose
// last read marker is older than this simply under-reports; it never blocks the
// UI task for an unbounded time.
constexpr size_t kSummaryScanRecords = 256;

// Bound on the tail scan updateStatus() uses to suppress redundant patches.
constexpr size_t kStatusPeekRecords = 96;

// Meshtastic's channel table size. Only used to keep a corrupt index out of
// channels.getName().
constexpr uint8_t kMaxChannelIndex = 7;

// ---------------------------------------------------------------------------
// Little-endian primitives
// ---------------------------------------------------------------------------

inline void put8(uint8_t *b, size_t &o, uint8_t v)
{
    b[o++] = v;
}

inline void put16(uint8_t *b, size_t &o, uint16_t v)
{
    b[o++] = (uint8_t)(v & 0xff);
    b[o++] = (uint8_t)(v >> 8);
}

inline void put32(uint8_t *b, size_t &o, uint32_t v)
{
    b[o++] = (uint8_t)(v & 0xff);
    b[o++] = (uint8_t)((v >> 8) & 0xff);
    b[o++] = (uint8_t)((v >> 16) & 0xff);
    b[o++] = (uint8_t)((v >> 24) & 0xff);
}

inline void putFloat(uint8_t *b, size_t &o, float v)
{
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(v), "float must be 32 bits");
    memcpy(&bits, &v, sizeof(bits));
    put32(b, o, bits);
}

inline uint8_t get8(const uint8_t *b, size_t &o)
{
    return b[o++];
}

inline uint16_t get16(const uint8_t *b, size_t &o)
{
    uint16_t v = (uint16_t)b[o] | ((uint16_t)b[o + 1] << 8);
    o += 2;
    return v;
}

inline uint32_t get32(const uint8_t *b, size_t &o)
{
    uint32_t v = (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
    o += 4;
    return v;
}

inline float getFloat(const uint8_t *b, size_t &o)
{
    uint32_t bits = get32(b, o);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

/// CRC-16/CCITT-FALSE: poly 0x1021, init 0xffff, no reflection, no final xor.
uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t bit = 0; bit < 8; bit++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// ---------------------------------------------------------------------------
// Status ordering
// ---------------------------------------------------------------------------

/// Position of a status in the forward-only progression. A patch is applied
/// only when it strictly increases this. Delivered outranks Failed on purpose;
/// Received sits at the top so nothing can rewrite an inbound message.
uint8_t statusRank(MsgStatus s)
{
    switch (s) {
    case MsgStatus::Unknown:
        return 0;
    case MsgStatus::Composing:
        return 1;
    case MsgStatus::Queued:
        return 2;
    case MsgStatus::Sent:
        return 3;
    case MsgStatus::Failed:
        return 4;
    case MsgStatus::Delivered:
        return 5;
    case MsgStatus::Received:
        return 6;
    }
    return 0;
}

bool validStatusByte(uint8_t v)
{
    return v <= (uint8_t)MsgStatus::Received;
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

/// Seals a record whose payload has been written at [kHeaderLen, payloadEnd).
/// Returns the total record length.
size_t sealRecord(uint8_t *buf, size_t payloadEnd, uint8_t type)
{
    const size_t total = payloadEnd + kTrailerLen;
    size_t h = 0;
    put8(buf, h, kMagic0);
    put8(buf, h, kMagic1);
    put8(buf, h, kFormatVersion);
    put8(buf, h, type);
    put16(buf, h, (uint16_t)total);

    size_t t = payloadEnd;
    put16(buf, t, crc16(buf, payloadEnd));
    put32(buf, t, (uint32_t)total);
    return total;
}

struct RecordView {
    uint8_t type = 0;
    const uint8_t *payload = nullptr;
    size_t payloadLen = 0;
};

/// Validates a complete record sitting in `buf`. `len` is what the caller
/// believes the record length to be (from the trailer on a backward walk, or
/// from the header on a forward scan); both stored copies must agree with it.
bool parseFrame(const uint8_t *buf, size_t len, RecordView &out)
{
    if (len < kMinRecordLen || len > kMaxRecordLen)
        return false;
    if (buf[0] != kMagic0 || buf[1] != kMagic1)
        return false;
    if (buf[2] != kFormatVersion)
        return false;

    size_t o = 4;
    const uint16_t headerLen = get16(buf, o);
    if (headerLen != len)
        return false;

    o = len - 6;
    const uint16_t storedCrc = get16(buf, o);
    const uint32_t trailerLen = get32(buf, o);
    if (trailerLen != len)
        return false;
    if (storedCrc != crc16(buf, len - kTrailerLen))
        return false;

    out.type = buf[3];
    out.payload = buf + kHeaderLen;
    out.payloadLen = len - kFrameLen;
    return true;
}

// ---------------------------------------------------------------------------
// Encode / decode
// ---------------------------------------------------------------------------

size_t encodeMessage(uint8_t *buf, const ChatMessage &m)
{
    // Clamp every length before it is written, so a caller that handed us an
    // unterminated array cannot produce a record we would later reject.
    size_t shortLen = strnlen(m.senderShort, kMaxShortName - 1);
    size_t longLen = strnlen(m.senderLong, kMaxLongName - 1);
    size_t textLen = m.textLen;
    if (textLen > kMaxTextLen)
        textLen = kMaxTextLen;
    // Honour an explicit textLen, but never read past a NUL the caller left.
    textLen = strnlen(m.text, textLen);

    size_t o = kHeaderLen;
    put32(buf, o, m.packetId);
    put32(buf, o, m.from);
    put32(buf, o, m.to);
    put32(buf, o, m.rxTime);
    put32(buf, o, m.uptimeMs);
    putFloat(buf, o, m.snr);
    put8(buf, o, m.channel);
    put8(buf, o, m.hopsAway);
    put8(buf, o, (uint8_t)m.rssi);
    put8(buf, o, (uint8_t)m.status);
    put8(buf, o, m.flags);
    put8(buf, o, (uint8_t)shortLen);
    put8(buf, o, (uint8_t)longLen);
    put16(buf, o, (uint16_t)textLen);

    memcpy(buf + o, m.senderShort, shortLen);
    o += shortLen;
    memcpy(buf + o, m.senderLong, longLen);
    o += longLen;
    memcpy(buf + o, m.text, textLen);
    o += textLen;

    return sealRecord(buf, o, kRecMessage);
}

size_t encodeStatus(uint8_t *buf, uint32_t packetId, MsgStatus status)
{
    size_t o = kHeaderLen;
    put32(buf, o, packetId);
    put8(buf, o, (uint8_t)status);
    return sealRecord(buf, o, kRecStatus);
}

size_t encodeRead(uint8_t *buf, uint32_t rxTime, uint32_t uptimeMs)
{
    size_t o = kHeaderLen;
    put32(buf, o, rxTime);
    put32(buf, o, uptimeMs);
    return sealRecord(buf, o, kRecRead);
}

/// Decodes a message payload. Every length is checked against its maximum and
/// against the bytes actually present before a single byte is copied.
bool decodeMessage(const RecordView &v, ChatMessage &out)
{
    if (v.payloadLen < kMsgFixedLen)
        return false;

    size_t o = 0;
    const uint8_t *p = v.payload;
    out = ChatMessage();
    out.packetId = get32(p, o);
    out.from = get32(p, o);
    out.to = get32(p, o);
    out.rxTime = get32(p, o);
    out.uptimeMs = get32(p, o);
    out.snr = getFloat(p, o);
    out.channel = get8(p, o);
    out.hopsAway = get8(p, o);
    out.rssi = (int8_t)get8(p, o);
    const uint8_t statusByte = get8(p, o);
    out.flags = get8(p, o);
    const size_t shortLen = get8(p, o);
    const size_t longLen = get8(p, o);
    const size_t textLen = get16(p, o);

    if (!validStatusByte(statusByte))
        return false;
    if (shortLen >= kMaxShortName || longLen >= kMaxLongName || textLen > kMaxTextLen)
        return false;
    if (kMsgFixedLen + shortLen + longLen + textLen != v.payloadLen)
        return false;

    out.status = (MsgStatus)statusByte;

    memcpy(out.senderShort, p + o, shortLen);
    out.senderShort[shortLen] = 0;
    o += shortLen;
    memcpy(out.senderLong, p + o, longLen);
    out.senderLong[longLen] = 0;
    o += longLen;
    memcpy(out.text, p + o, textLen);
    out.text[textLen] = 0;
    out.textLen = (uint16_t)textLen;
    return true;
}

bool decodeStatus(const RecordView &v, uint32_t &packetId, MsgStatus &status)
{
    if (v.payloadLen != kStatusPayloadLen)
        return false;
    size_t o = 0;
    packetId = get32(v.payload, o);
    const uint8_t statusByte = get8(v.payload, o);
    if (!validStatusByte(statusByte))
        return false;
    status = (MsgStatus)statusByte;
    return true;
}

// ---------------------------------------------------------------------------
// Patch table
// ---------------------------------------------------------------------------

/// Status patches gathered during a backward walk, keyed by packetId. Fixed
/// capacity, no allocation. Entries are seen newest-first, so on overflow the
/// older (less authoritative) patch is the one dropped.
class PatchTable
{
  public:
    void note(uint32_t packetId, MsgStatus status)
    {
        if (!packetId)
            return;
        for (size_t i = 0; i < mCount; i++) {
            if (mIds[i] == packetId) {
                if (statusRank(status) > statusRank(mStatus[i]))
                    mStatus[i] = status;
                return;
            }
        }
        if (mCount == kCapacity)
            return; // full: this patch is older than everything already held
        mIds[mCount] = packetId;
        mStatus[mCount] = status;
        mCount++;
    }

    bool lookup(uint32_t packetId, MsgStatus &status) const
    {
        if (!packetId)
            return false;
        for (size_t i = 0; i < mCount; i++) {
            if (mIds[i] == packetId) {
                status = mStatus[i];
                return true;
            }
        }
        return false;
    }

  private:
    static constexpr size_t kCapacity = 48;
    uint32_t mIds[kCapacity] = {0};
    MsgStatus mStatus[kCapacity] = {MsgStatus::Unknown};
    size_t mCount = 0;
};

/// Folds everything learned from newer records into one decoded message.
void foldInto(ChatMessage &m, const PatchTable &patches, bool pastReadMarker)
{
    MsgStatus patched;
    if (patches.lookup(m.packetId, patched) && statusRank(patched) > statusRank(m.status))
        m.status = patched;
    if (pastReadMarker)
        m.flags &= (uint8_t)~kFlagUnread;
}

// ---------------------------------------------------------------------------
// Filesystem plumbing
// ---------------------------------------------------------------------------

/// spiLock does not exist until initSPI() has run, and ChatStore::begin() is
/// deliberately allowed to run before that.
struct FsGuard {
    FsGuard()
    {
        if (spiLock)
            spiLock->lock();
    }
    ~FsGuard()
    {
        if (spiLock)
            spiLock->unlock();
    }
    FsGuard(const FsGuard &) = delete;
    FsGuard &operator=(const FsGuard &) = delete;
};

/// Reads the record that ends at byte offset `end`, using the trailing length.
/// Returns the record length, or 0 if there is nothing valid there. This is the
/// one primitive the whole backward walk is built on.
size_t readRecordEndingAt(File &f, uint32_t end, uint8_t *buf, RecordView &view)
{
    if (end < kMinRecordLen)
        return 0;
    if (!f.seek(end - 4))
        return 0;

    uint8_t lenBuf[4];
    if (f.read(lenBuf, sizeof(lenBuf)) != sizeof(lenBuf))
        return 0;

    size_t o = 0;
    const uint32_t recLen = get32(lenBuf, o);
    if (recLen < kMinRecordLen || recLen > kMaxRecordLen || recLen > end)
        return 0;
    if (!f.seek(end - recLen))
        return 0;
    if (f.read(buf, recLen) != recLen)
        return 0;
    if (!parseFrame(buf, recLen, view))
        return 0;
    return recLen;
}

/// Rewrites `path` keeping only bytes [from, to), by way of a temp file and a
/// rename. Used both by recovery (from = 0) and by compaction.
bool rewriteRange(const char *path, uint32_t from, uint32_t to)
{
    char tmp[kPathLen];
    if (snprintf(tmp, sizeof(tmp), "%s.new", path) >= (int)sizeof(tmp)) {
        LOG_ERROR("ChatStore: path too long for temp file: %s", path);
        return false;
    }

    File src = FSCom.open(path, FILE_O_READ);
    if (!src)
        return false;
    if (from && !src.seek(from)) {
        src.close();
        return false;
    }

    File dst = FSCom.open(tmp, FILE_O_WRITE);
    if (!dst) {
        src.close();
        return false;
    }

    uint8_t chunk[256];
    bool ok = true;
    uint32_t left = (to > from) ? (to - from) : 0;
    while (left) {
        const size_t want = left < sizeof(chunk) ? (size_t)left : sizeof(chunk);
        const size_t got = src.read(chunk, want);
        if (got != want) {
            ok = false;
            break;
        }
        if (dst.write(chunk, got) != got) {
            ok = false;
            break;
        }
        left -= (uint32_t)got;
    }

    dst.flush();
    dst.close();
    src.close();

    if (!ok) {
        FSCom.remove(tmp);
        return false;
    }
    // LittleFS rename replaces the destination atomically, so a power loss here
    // leaves either the old file or the new one, never a half of either.
    if (!FSCom.rename(tmp, path)) {
        LOG_ERROR("ChatStore: rename %s -> %s failed", tmp, path);
        FSCom.remove(tmp);
        return false;
    }
    return true;
}

/// Full forward scan. Follows each record's header length from offset 0 and
/// stops at the first record that does not validate; everything from there to
/// EOF is torn or rotted and is dropped.
bool verifyLocked(const char *path, StoreStats &stats)
{
    stats = StoreStats();

    File f = FSCom.open(path, FILE_O_READ);
    if (!f)
        return true; // no file is a valid empty thread
    if (f.isDirectory()) {
        f.close();
        return false;
    }

    const uint32_t size = (uint32_t)f.size();
    stats.bytes = size;

    uint8_t buf[kRecordBufLen];
    uint32_t pos = 0;
    uint32_t lastGood = 0;

    while (pos + kMinRecordLen <= size) {
        if (!f.seek(pos))
            break;
        uint8_t head[kHeaderLen];
        if (f.read(head, sizeof(head)) != sizeof(head))
            break;
        if (head[0] != kMagic0 || head[1] != kMagic1 || head[2] != kFormatVersion)
            break;

        size_t o = 4;
        const uint32_t recLen = get16(head, o);
        if (recLen < kMinRecordLen || recLen > kMaxRecordLen || pos + recLen > size)
            break;

        if (!f.seek(pos))
            break;
        if (f.read(buf, recLen) != recLen)
            break;

        RecordView view;
        if (!parseFrame(buf, recLen, view))
            break;

        stats.records++;
        pos += recLen;
        lastGood = pos;
    }

    f.close();

    if (lastGood == size)
        return true;

    stats.truncatedBytes = size - lastGood;
    stats.recovered = true;
    LOG_WARN("ChatStore: %s has a torn tail, dropping %u byte(s) after record %u", path, (unsigned)stats.truncatedBytes,
             (unsigned)stats.records);

    if (lastGood == 0) {
        if (!FSCom.remove(path)) {
            LOG_ERROR("ChatStore: could not remove unreadable %s", path);
            return false;
        }
        stats.bytes = 0;
        return true;
    }
    if (!rewriteRange(path, 0, lastGood))
        return false;
    stats.bytes = lastGood;
    return true;
}

// Threads validated since boot. A hit skips even the cheap tail check.
constexpr size_t kVerifyCacheSize = 8;
uint32_t gVerified[kVerifyCacheSize] = {0};
size_t gVerifiedNext = 0;

uint32_t pathHash(const char *path)
{
    uint32_t h = 2166136261u; // FNV-1a
    for (const char *p = path; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    return h ? h : 1u; // 0 marks an empty slot
}

/// Lazy per-thread validation, run on first access rather than at boot. The
/// common case costs one seek and one record read: because the store only
/// appends, damage can only be at the tail, so a sound last record means a
/// sound file. Only when that fails do we pay for the full forward scan.
void ensureVerifiedLocked(const char *path)
{
    const uint32_t h = pathHash(path);
    for (size_t i = 0; i < kVerifyCacheSize; i++) {
        if (gVerified[i] == h)
            return;
    }

    File f = FSCom.open(path, FILE_O_READ);
    bool sound = true;
    if (f) {
        const uint32_t size = (uint32_t)f.size();
        if (size) {
            uint8_t buf[kRecordBufLen];
            RecordView view;
            sound = readRecordEndingAt(f, size, buf, view) != 0;
        }
        f.close();
    }

    if (!sound) {
        StoreStats stats;
        verifyLocked(path, stats);
    }

    gVerified[gVerifiedNext] = h;
    gVerifiedNext = (gVerifiedNext + 1) % kVerifyCacheSize;
}

/// Backward walk shared by readTail() and readBefore(). `beforeUptimeMs` of 0
/// means "no filter"; otherwise only messages strictly older than it are
/// collected. Messages land in `out` newest-last (chronological order).
size_t scanBackLocked(const char *path, uint32_t beforeUptimeMs, ChatMessage *out, size_t max)
{
    File f = FSCom.open(path, FILE_O_READ);
    if (!f)
        return 0;
    if (f.isDirectory()) {
        f.close();
        return 0;
    }

    uint32_t pos = (uint32_t)f.size();
    uint8_t buf[kRecordBufLen];
    PatchTable patches;
    bool pastReadMarker = false;
    size_t collected = 0;

    // Fill `out` from the back: the walk yields newest first, but callers want
    // oldest first, and this avoids a second buffer to reverse through.
    while (pos >= kMinRecordLen && collected < max) {
        RecordView view;
        const size_t recLen = readRecordEndingAt(f, pos, buf, view);
        if (!recLen) {
            // ensureVerifiedLocked() should have made this impossible; a hit
            // here means the file changed under us or the FS returned short.
            LOG_WARN("ChatStore: stopping walk of %s at offset %u", path, (unsigned)pos);
            break;
        }

        if (view.type == kRecStatus) {
            uint32_t patchId = 0;
            MsgStatus patchStatus = MsgStatus::Unknown;
            if (decodeStatus(view, patchId, patchStatus))
                patches.note(patchId, patchStatus);
        } else if (view.type == kRecRead) {
            pastReadMarker = true;
        } else if (view.type == kRecMessage) {
            ChatMessage &slot = out[max - 1 - collected];
            if (decodeMessage(view, slot) && (!beforeUptimeMs || slot.uptimeMs < beforeUptimeMs)) {
                foldInto(slot, patches, pastReadMarker);
                collected++;
            }
        }
        // Unknown record types are skipped by length: that is what the version
        // byte and the length header buy us for future formats.

        pos -= (uint32_t)recLen;
    }

    f.close();

    if (collected && collected < max)
        memmove(out, out + (max - collected), collected * sizeof(ChatMessage));
    return collected;
}

/// Current effective status of one message, from a bounded tail scan. Used only
/// to suppress patch records that would not move the message forward.
bool peekStatusLocked(const char *path, uint32_t packetId, MsgStatus &out)
{
    File f = FSCom.open(path, FILE_O_READ);
    if (!f)
        return false;

    uint32_t pos = (uint32_t)f.size();
    uint8_t buf[kRecordBufLen];
    MsgStatus best = MsgStatus::Unknown;
    bool found = false;

    for (size_t seen = 0; seen < kStatusPeekRecords && pos >= kMinRecordLen; seen++) {
        RecordView view;
        const size_t recLen = readRecordEndingAt(f, pos, buf, view);
        if (!recLen)
            break;

        if (view.type == kRecStatus) {
            uint32_t id = 0;
            MsgStatus st = MsgStatus::Unknown;
            if (decodeStatus(view, id, st) && id == packetId) {
                if (!found || statusRank(st) > statusRank(best))
                    best = st;
                found = true;
            }
        } else if (view.type == kRecMessage) {
            ChatMessage m;
            if (decodeMessage(view, m) && m.packetId == packetId) {
                if (!found || statusRank(m.status) > statusRank(best))
                    best = m.status;
                found = true;
                break; // the message itself is the oldest thing that can matter
            }
        }
        pos -= (uint32_t)recLen;
    }

    f.close();
    if (found)
        out = best;
    return found;
}

bool appendRecordLocked(const char *path, const uint8_t *rec, size_t len)
{
    File f = FSCom.open(path, FILE_O_APPEND);
    if (!f) {
        LOG_ERROR("ChatStore: cannot open %s for append", path);
        return false;
    }
    const size_t wrote = f.write(rec, len);
    f.flush();
    f.close();

    if (wrote != len) {
        // The partial record is a torn tail like any other and is dropped by the
        // next verify; earlier history is untouched.
        LOG_ERROR("ChatStore: short write to %s (%u of %u)", path, (unsigned)wrote, (unsigned)len);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Directory enumeration
// ---------------------------------------------------------------------------

const char *kChannelDir = "/pgros/ch";
const char *kDirectDir = "/pgros/dm";

/// Strips any directory part the FS backend chose to include in name().
const char *basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/// "3.log" -> channel 3, "a1b2c3d4.log" -> peer 0xa1b2c3d4. Rejects anything
/// else, including our own ".log.new" temp files.
bool threadFromName(const char *name, bool direct, ThreadId &out)
{
    const size_t len = strlen(name);
    if (len < 5 || strcmp(name + len - 4, ".log") != 0)
        return false;

    const size_t stemLen = len - 4;
    char stem[16];
    if (stemLen >= sizeof(stem))
        return false;
    memcpy(stem, name, stemLen);
    stem[stemLen] = 0;

    char *end = nullptr;
    const unsigned long value = strtoul(stem, &end, direct ? 16 : 10);
    if (!end || *end)
        return false;

    if (direct) {
        if (stemLen != 8)
            return false;
        out = ThreadId::dm((uint32_t)value);
    } else {
        if (value > kMaxChannelIndex)
            return false;
        out = ThreadId::broadcast((uint8_t)value);
    }
    return true;
}

/// Enumerates both thread directories into a caller-supplied array. Takes the
/// FS lock itself and releases it before returning, so callers are free to open
/// files afterwards without nesting a non-recursive mutex.
size_t collectThreads(ThreadId *out, size_t max, uint32_t *sizesOut = nullptr)
{
    FsGuard guard;
    size_t count = 0;

    for (int pass = 0; pass < 2 && count < max; pass++) {
        const bool direct = (pass == 1);
        File dir = FSCom.open(direct ? kDirectDir : kChannelDir, FILE_O_READ);
        if (!dir)
            continue;
        if (!dir.isDirectory()) {
            dir.close();
            continue;
        }

        File entry = dir.openNextFile();
        // The name()[0] test mirrors FSCommon.cpp: the nRF52 LittleFS glue can
        // hand back an entry with an empty name.
        while (entry && entry.name()[0] && count < max) {
            if (!entry.isDirectory()) {
                ThreadId id;
                if (threadFromName(basename(entry.name()), direct, id)) {
                    if (sizesOut)
                        sizesOut[count] = (uint32_t)entry.size();
                    out[count++] = id;
                }
            }
            entry.close();
            entry = dir.openNextFile();
        }
        if (entry)
            entry.close();
        dir.close();
    }

    return count;
}

constexpr size_t kMaxEnumeratedThreads = 48;

} // namespace

// ===========================================================================
// ChatStore
// ===========================================================================

void ChatStore::pathFor(const ThreadId &thread, char *out, size_t outLen)
{
    if (!out || !outLen)
        return;
    if (thread.direct)
        snprintf(out, outLen, "%s/%08x.log", kDirectDir, (unsigned)thread.peer);
    else
        snprintf(out, outLen, "%s/%u.log", kChannelDir, (unsigned)thread.channel);
}

bool ChatStore::begin()
{
    if (mReady)
        return true;

    FsGuard guard;

    // fsInit() usually got here first; mounting twice is a no-op, and doing it
    // here means the store also works if it is brought up before fsInit().
    if (!FSBegin()) {
        LOG_ERROR("ChatStore: filesystem mount failed");
        return false;
    }

    // Cheap by design: three mkdirs and nothing else. No thread file is opened,
    // scanned or validated here -- that happens on first access instead, so it
    // never sits on the boot path.
    FSCom.mkdir("/pgros");
    FSCom.mkdir(kChannelDir);
    FSCom.mkdir(kDirectDir);

    mReady = true;
    LOG_INFO("ChatStore: ready (%s, %s)", kChannelDir, kDirectDir);
    return true;
}

bool ChatStore::append(const ThreadId &thread, const ChatMessage &msg)
{
    if (!mReady)
        return false;

    ChatMessage stored = msg;
    if (!stored.uptimeMs)
        stored.uptimeMs = millis();
    if (stored.textLen > kMaxTextLen)
        stored.textLen = kMaxTextLen;

    char path[kPathLen];
    pathFor(thread, path, sizeof(path));

    uint8_t buf[kRecordBufLen];
    const size_t len = encodeMessage(buf, stored);

    FsGuard guard;
    ensureVerifiedLocked(path);
    return appendRecordLocked(path, buf, len);
}

bool ChatStore::updateStatus(const ThreadId &thread, uint32_t packetId, MsgStatus status)
{
    if (!mReady || !packetId)
        return false;

    char path[kPathLen];
    pathFor(thread, path, sizeof(path));

    FsGuard guard;
    ensureVerifiedLocked(path);

    // A patch that cannot move the message forward is not worth the flash write.
    // If the message is not in the peek window we append anyway; the fold at
    // read time still refuses to regress it.
    MsgStatus current = MsgStatus::Unknown;
    if (peekStatusLocked(path, packetId, current) && statusRank(status) <= statusRank(current))
        return true;

    uint8_t buf[kRecordBufLen];
    const size_t len = encodeStatus(buf, packetId, status);
    return appendRecordLocked(path, buf, len);
}

bool ChatStore::markThreadRead(const ThreadId &thread)
{
    if (!mReady)
        return false;

    char path[kPathLen];
    pathFor(thread, path, sizeof(path));

    uint8_t buf[kRecordBufLen];
    const size_t len = encodeRead(buf, getValidTime(RTCQualityDevice), millis());

    FsGuard guard;
    ensureVerifiedLocked(path);
    return appendRecordLocked(path, buf, len);
}

size_t ChatStore::readTail(const ThreadId &thread, ChatMessage *out, size_t max)
{
    if (!mReady || !out || !max)
        return 0;

    char path[kPathLen];
    pathFor(thread, path, sizeof(path));

    FsGuard guard;
    ensureVerifiedLocked(path);
    return scanBackLocked(path, 0, out, max);
}

size_t ChatStore::readBefore(const ThreadId &thread, uint32_t beforeUptimeMs, ChatMessage *out, size_t max)
{
    if (!mReady || !out || !max)
        return 0;

    char path[kPathLen];
    pathFor(thread, path, sizeof(path));

    FsGuard guard;
    ensureVerifiedLocked(path);
    // beforeUptimeMs == 0 degenerates to readTail(), which is what a caller
    // paging upward from an empty view wants.
    return scanBackLocked(path, beforeUptimeMs, out, max);
}

bool ChatStore::summarise(const ThreadId &thread, ThreadSummary &out)
{
    out = ThreadSummary();
    out.id = thread;
    if (!mReady)
        return false;

    // A default title so the list still renders if the thread has no usable
    // name snapshot in the scanned window.
    if (thread.direct)
        snprintf(out.title, sizeof(out.title), "!%08x", (unsigned)thread.peer);
    else if (thread.channel <= kMaxChannelIndex && channels.getName(thread.channel) && channels.getName(thread.channel)[0])
        snprintf(out.title, sizeof(out.title), "%s", channels.getName(thread.channel));
    else
        snprintf(out.title, sizeof(out.title), "Channel %u", (unsigned)thread.channel);

    char path[kPathLen];
    pathFor(thread, path, sizeof(path));

    FsGuard guard;
    ensureVerifiedLocked(path);

    File f = FSCom.open(path, FILE_O_READ);
    if (!f)
        return false;
    if (f.isDirectory()) {
        f.close();
        return false;
    }

    uint32_t pos = (uint32_t)f.size();
    uint8_t buf[kRecordBufLen];
    PatchTable patches;
    bool pastReadMarker = false;
    bool haveNewest = false;
    bool haveTitle = false;
    uint32_t unread = 0;

    // Bounded: the newest message plus however many unread messages sit above
    // the newest read marker. Bodies are not retained, only a preview.
    for (size_t seen = 0; seen < kSummaryScanRecords && pos >= kMinRecordLen; seen++) {
        RecordView view;
        const size_t recLen = readRecordEndingAt(f, pos, buf, view);
        if (!recLen)
            break;

        if (view.type == kRecStatus) {
            uint32_t id = 0;
            MsgStatus st = MsgStatus::Unknown;
            if (decodeStatus(view, id, st))
                patches.note(id, st);
        } else if (view.type == kRecRead) {
            pastReadMarker = true;
            if (haveNewest && haveTitle)
                break;
        } else if (view.type == kRecMessage) {
            ChatMessage m;
            if (decodeMessage(view, m)) {
                foldInto(m, patches, pastReadMarker);

                if (!haveNewest) {
                    haveNewest = true;
                    out.lastActivity = m.rxTime ? m.rxTime : m.uptimeMs;
                    out.lastWasOutbound = (m.flags & kFlagOutbound) != 0;
                    snprintf(out.preview, sizeof(out.preview), "%s", m.text);
                    snprintf(out.lastSenderShort, sizeof(out.lastSenderShort), "%s", m.senderShort);
                }
                // For a DM the peer's own name snapshot is the thread title.
                if (!haveTitle && thread.direct && m.from == thread.peer && m.senderLong[0]) {
                    snprintf(out.title, sizeof(out.title), "%s", m.senderLong);
                    haveTitle = true;
                }
                if ((m.flags & kFlagUnread) && !(m.flags & kFlagOutbound))
                    unread++;
            }
        }

        pos -= (uint32_t)recLen;
    }

    f.close();
    out.unread = (unread > 0xffff) ? 0xffff : (uint16_t)unread;
    return haveNewest;
}

size_t ChatStore::listThreads(ThreadSummary *out, size_t max)
{
    if (!mReady || !out || !max)
        return 0;

    ThreadId ids[kMaxEnumeratedThreads];
    // collectThreads() releases the FS lock before returning, so each
    // summarise() below can take it for itself.
    const size_t found = collectThreads(ids, kMaxEnumeratedThreads);

    size_t count = 0;
    for (size_t i = 0; i < found && count < max; i++) {
        ThreadSummary summary;
        if (summarise(ids[i], summary))
            out[count++] = summary;
    }

    std::sort(out, out + count,
              [](const ThreadSummary &a, const ThreadSummary &b) { return a.lastActivity > b.lastActivity; });
    return count;
}

uint16_t ChatStore::unreadCount(const ThreadId &thread)
{
    ThreadSummary summary;
    if (!summarise(thread, summary))
        return 0;
    return summary.unread;
}

bool ChatStore::verify(const ThreadId &thread, StoreStats &stats)
{
    stats = StoreStats();
    if (!mReady)
        return false;

    char path[kPathLen];
    pathFor(thread, path, sizeof(path));

    FsGuard guard;
    const bool ok = verifyLocked(path, stats);
    if (ok) {
        // A full scan supersedes whatever the lazy check concluded.
        const uint32_t h = pathHash(path);
        bool present = false;
        for (size_t i = 0; i < kVerifyCacheSize && !present; i++)
            present = (gVerified[i] == h);
        if (!present) {
            gVerified[gVerifiedNext] = h;
            gVerifiedNext = (gVerifiedNext + 1) % kVerifyCacheSize;
        }
    }
    return ok;
}

bool ChatStore::compact(const ThreadId &thread, uint32_t keepRecords)
{
    if (!mReady)
        return false;

    char path[kPathLen];
    pathFor(thread, path, sizeof(path));

    FsGuard guard;
    ensureVerifiedLocked(path);

    File f = FSCom.open(path, FILE_O_READ);
    if (!f)
        return true; // nothing to compact
    if (f.isDirectory()) {
        f.close();
        return false;
    }

    const uint32_t size = (uint32_t)f.size();
    uint32_t pos = size;
    uint32_t keepFrom = 0;
    uint32_t kept = 0;
    bool reachedStart = true;
    uint8_t buf[kRecordBufLen];

    // Walk back until `keepRecords` message records have gone by; that record's
    // offset is where the new file starts. Every status patch and read marker
    // that matters is necessarily newer than the message it refers to, so it is
    // inside the copied range already and nothing has to be synthesised.
    while (pos >= kMinRecordLen) {
        RecordView view;
        const size_t recLen = readRecordEndingAt(f, pos, buf, view);
        if (!recLen)
            break;
        pos -= (uint32_t)recLen;

        if (view.type == kRecMessage) {
            kept++;
            if (kept >= keepRecords) {
                keepFrom = pos;
                reachedStart = false;
                break;
            }
        }
    }

    f.close();

    if (reachedStart || keepFrom == 0)
        return true; // the whole file is already within budget

    LOG_INFO("ChatStore: compacting %s, dropping %u of %u byte(s)", path, (unsigned)keepFrom, (unsigned)size);
    return rewriteRange(path, keepFrom, size);
}

void ChatStore::compactIfNeeded()
{
    if (!mReady)
        return;

    ThreadId ids[kMaxEnumeratedThreads];
    uint32_t sizes[kMaxEnumeratedThreads] = {0};
    const size_t found = collectThreads(ids, kMaxEnumeratedThreads, sizes);

    for (size_t i = 0; i < found; i++) {
        if (sizes[i] > kCompactThresholdBytes)
            compact(ids[i], kCompactKeepRecords);
    }
}

bool ChatStore::erase(const ThreadId &thread)
{
    if (!mReady)
        return false;

    char path[kPathLen];
    pathFor(thread, path, sizeof(path));

    FsGuard guard;
    if (!FSCom.exists(path))
        return true;
    if (!FSCom.remove(path)) {
        LOG_ERROR("ChatStore: could not remove %s", path);
        return false;
    }
    return true;
}

uint32_t ChatStore::bytesUsed()
{
    if (!mReady)
        return 0;

    ThreadId ids[kMaxEnumeratedThreads];
    uint32_t sizes[kMaxEnumeratedThreads] = {0};
    const size_t found = collectThreads(ids, kMaxEnumeratedThreads, sizes);

    uint32_t total = 0;
    for (size_t i = 0; i < found; i++)
        total += sizes[i];
    return total;
}

ChatStore chatStore;

} // namespace pgros
