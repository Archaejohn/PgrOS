//
// PgrOS device policy -- persistence.
//
// ============================================================================
// ON-DISK FORMAT  ("PGPL", PgrOS policy blob)
// ============================================================================
//
// One file, /pgros/policy.bin, rewritten whole on every save. It is a few dozen
// bytes; there is no reason for anything cleverer than a full rewrite.
//
//   off  size  field
//   ---  ----  --------------------------------------------------------------
//     0     4  magic          'P','G','P','L'
//     4     2  blobVersion    framing version, kBlobVersion
//     6     2  policyVersion  copy of Policy::version, for migrations
//     8     2  payloadLen     sizeof(Policy) as written
//    10     2  crc16          CRC-16/CCITT-FALSE over the payload
//    12     N  payload        raw Policy bytes
//
// The payload is a straight memcpy of the Policy struct rather than a
// field-by-field encoding. That is a deliberate trade: Policy is trivially
// copyable and built only from uint8/uint16/bool, so its layout is stable and
// its padding is minimal -- but it does tie the blob to the struct's layout.
// ANY change to Policy's members must bump Policy::version in the header.
// payloadLen and the CRC turn a mismatch into a clean "reset to defaults"
// rather than into garbage settings.
//
// Writes go through Meshtastic's SafeFile, which writes a .tmp, reads it back
// and compares an XOR hash, and only then renames over the real file. A power
// loss mid-save therefore leaves the previous policy intact rather than a
// truncated one. The CRC above is a second, independent check: SafeFile's hash
// verifies the write round-tripped, the CRC verifies the bytes we are about to
// trust at load time.
//
// Silence note: the defaults in Policy.h are all-silent by design. This file
// must never invent a "sensible" non-silent fallback on a corrupt read -- a
// device that loses its settings goes back to making no noise, not to beeping.
//

#ifdef PGROS

#include "Policy.h"

#include "FSCommon.h"
#include "SafeFile.h"
#include "configuration.h"

#include <stdio.h>
#include <string.h>

namespace pgros
{

namespace
{

constexpr const char *kPolicyDir = "/pgros";
constexpr const char *kPolicyPath = "/pgros/policy.bin";

constexpr uint16_t kBlobVersion = 1;
constexpr size_t kHeaderLen = 12;

// The Policy::version value this build knows how to read. Kept separate from
// kBlobVersion: the framing can stay put while the struct evolves.
constexpr uint16_t kPolicyVersion = 1;

/// CRC-16/CCITT-FALSE. Same polynomial and seed ChatStore uses, so there is one
/// CRC in PgrOS and not two.
uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

inline void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

inline uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

inline bool audible(AlertMode m)
{
    return m == AlertMode::Sound || m == AlertMode::Both;
}

} // namespace

PolicyStore policy;

// ---------------------------------------------------------------------------

bool PolicyStore::begin()
{
    // Deliberately infallible from the caller's point of view. A preferences
    // file is not worth a failed boot, and the defaults are safe (silent) by
    // construction, so every error path below ends in "use defaults, carry on".
    //
    // Must run after fsInit()/initSPI(): FSCom assumes the filesystem is
    // mounted, and SafeFile (used by save()) assumes spiLock exists.

    mPolicy = Policy();
    mDirty = false;

    if (!FSCom.exists(kPolicyPath)) {
        LOG_INFO("PgrOS: no policy file, using silent defaults");
        mDirty = true; // persisted by the first save(), not on the boot path
        return true;
    }

    File f = FSCom.open(kPolicyPath, FILE_O_READ);
    if (!f) {
        LOG_WARN("PgrOS: policy open failed, using defaults");
        mDirty = true;
        return true;
    }

    uint8_t header[kHeaderLen];
    if (f.read(header, kHeaderLen) != (int)kHeaderLen) {
        LOG_WARN("PgrOS: policy file truncated, using defaults");
        f.close();
        mDirty = true;
        return true;
    }

    const bool magicOk = header[0] == 'P' && header[1] == 'G' && header[2] == 'P' && header[3] == 'L';
    const uint16_t blobVersion = get16(header + 4);
    const uint16_t policyVersion = get16(header + 6);
    const uint16_t payloadLen = get16(header + 8);
    const uint16_t wantCrc = get16(header + 10);

    if (!magicOk || blobVersion != kBlobVersion) {
        LOG_WARN("PgrOS: policy blob v%u unrecognised, resetting to defaults", (unsigned)blobVersion);
        f.close();
        mDirty = true;
        return true;
    }

    // Version migration hook. There is exactly one version so far, so the only
    // honest thing to do with anything else is reset. When a v2 arrives, read
    // the v1 payload into a v1-shaped struct here and translate it; do not
    // reinterpret a differently sized blob in place.
    if (policyVersion != kPolicyVersion || payloadLen != (uint16_t)sizeof(Policy)) {
        LOG_WARN("PgrOS: policy v%u/%uB != expected v%u/%uB, resetting", (unsigned)policyVersion, (unsigned)payloadLen,
                 (unsigned)kPolicyVersion, (unsigned)sizeof(Policy));
        f.close();
        mDirty = true;
        return true;
    }

    Policy loaded;
    if (f.read(reinterpret_cast<uint8_t *>(&loaded), sizeof(loaded)) != (int)sizeof(loaded)) {
        LOG_WARN("PgrOS: policy payload short, using defaults");
        f.close();
        mPolicy = Policy();
        mDirty = true;
        return true;
    }
    f.close();

    if (crc16(reinterpret_cast<const uint8_t *>(&loaded), sizeof(loaded)) != wantCrc) {
        LOG_WARN("PgrOS: policy CRC mismatch, using defaults");
        mDirty = true;
        return true;
    }

    // Belt and braces: these bytes came off flash, so clamp the fields where an
    // out-of-range value would surface as a bug somewhere else (a volume of 200,
    // an AlertMode of 9 falling through a switch).
    if (loaded.volume > 10)
        loaded.volume = 10;
    if ((uint8_t)loaded.messageAlert > (uint8_t)AlertMode::Both)
        loaded.messageAlert = AlertMode::Off;
    if ((uint8_t)loaded.dmAlert > (uint8_t)AlertMode::Both)
        loaded.dmAlert = AlertMode::Off;
    if ((uint8_t)loaded.theme > (uint8_t)ThemeMode::Auto)
        loaded.theme = ThemeMode::Dark;

    mPolicy = loaded;
    mDirty = false;
    LOG_INFO("PgrOS: policy loaded, audible output %s", anyAudibleOutput() ? "enabled" : "off");
    return true;
}

bool PolicyStore::save()
{
    // Idempotent and cheap; ChatStore, which also creates it, may not have run.
    FSCom.mkdir(kPolicyDir);

    mPolicy.version = kPolicyVersion;

    uint8_t header[kHeaderLen];
    header[0] = 'P';
    header[1] = 'G';
    header[2] = 'P';
    header[3] = 'L';
    put16(header + 4, kBlobVersion);
    put16(header + 6, mPolicy.version);
    put16(header + 8, (uint16_t)sizeof(Policy));
    put16(header + 10, crc16(reinterpret_cast<const uint8_t *>(&mPolicy), sizeof(Policy)));

    // fullAtomic=true: the file is tiny, so we can afford to keep the old copy
    // on disk until the new one has been written and read back successfully.
    SafeFile file(kPolicyPath, true);
    file.write(header, sizeof(header));
    file.write(reinterpret_cast<const uint8_t *>(&mPolicy), sizeof(Policy));

    if (!file.close()) {
        LOG_ERROR("PgrOS: policy save failed, previous file kept");
        return false;
    }

    mDirty = false;
    return true;
}

bool PolicyStore::reset()
{
    mPolicy = Policy();
    mDirty = true;
    return save();
}

bool PolicyStore::anyAudibleOutput() const
{
    // Haptics are not audible output: keyHaptic, and an AlertMode of Haptic, are
    // deliberately excluded. This predicate answers "can this device make a
    // noise", which is what the amplifier gating and the settings UI care about.
    return mPolicy.bootChime || mPolicy.keyClick || audible(mPolicy.messageAlert) || audible(mPolicy.dmAlert);
}

} // namespace pgros

#endif // PGROS
