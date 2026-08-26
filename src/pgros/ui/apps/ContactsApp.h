#pragma once
//
// The node list, and the way a direct message gets started.
//
// Deliberately the same shape as MessagesApp: same 38px rows, same avatar chip
// on the left, same right-hand column, same keys. The two list screens are the
// only lists on the device, and a user who has learned one has learned both.
//
// Row anatomy, 480 x 38:
//
//   +----+---------------------------------------------+---------+
//   | KE4| Kevin's Pager                               |     12m |
//   |    | 2 hops  .  !a1b2c3d4                        |    8 dB |
//   +----+---------------------------------------------+---------+
//
// THREADING. This screen never touches NodeDB. NodeDB::meshNodes is an
// unguarded vector that reallocates on insert, so the mesh task publishes a
// frozen snapshot (MeshBridge::refreshNodes, called from the throttled NodeDB
// observer) and this screen reads a copy of it through MeshBridge::listNodes().
// See the seqlock note in core/MeshBridge.h.

#include "core/EventBus.h"
#include "core/MeshBridge.h"
#include "ui/App.h"
#include <stdint.h>

namespace pgros {

class ContactsApp : public App
{
  public:
    AppId id() const override { return AppId::Contacts; }
    const char *title() const override { return "Contacts"; }

    void onCreate(lv_obj_t *parent) override;
    void onShow(const AppArgs &args) override;
    bool onEvent(const Event &ev) override;
    bool onKey(uint32_t key) override;

  private:
    // The widget pool built at boot. Matches the snapshot depth so the list can
    // never be truncated below what the mesh task publishes.
    static constexpr uint8_t kMaxNodes = (uint8_t)MeshBridge::kNodeSnapshotMax;

    // A NodeUpdated storm on a busy mesh is already throttled at the source;
    // this is the second line of defence so a reload cannot land every frame.
    static constexpr uint32_t kReloadThrottleMs = 1000;

    struct RowView {
        lv_obj_t *obj = nullptr;
        lv_obj_t *avatar = nullptr;
        lv_obj_t *initials = nullptr;
        lv_obj_t *name = nullptr;
        lv_obj_t *detail = nullptr;
        lv_obj_t *time = nullptr;
        lv_obj_t *signal = nullptr;
    };

    void buildList(lv_obj_t *parent);
    void buildEmptyState(lv_obj_t *parent);

    void reload();
    void bindRow(uint8_t index);
    void applySelection(bool scroll);
    void moveSelection(int8_t delta);
    void openSelected();

    lv_obj_t *mList = nullptr;
    lv_obj_t *mEmpty = nullptr;

    RowView mRows[kMaxNodes];
    NodeBrief mNodes[kMaxNodes];
    uint8_t mCount = 0;
    uint8_t mSelected = 0;
    uint32_t mLastReloadMs = 0;
};

extern ContactsApp contactsApp;

} // namespace pgros
