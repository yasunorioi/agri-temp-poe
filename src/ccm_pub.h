// ccm_pub.h — optional UECS-CCM export (off by default), one slot per packet.
//
// agri-temp-poe is agriha-MQTT-native; this path exists only for the "drop our
// probe into an existing ArSprout greenhouse" case. Enable via /config (ccm_en).
//
// ONE <DATA> per packet: UECS allows several per envelope, but ArSprout's
// receiver keeps only the last one and silently drops the rest (this cost
// agri-flow / agri-amp weeks — see AgriCCM.h). So each active slot gets its own
// envelope. The core's ccmSend() emits to the limited broadcast address (what
// ArSprout actually receives) and the multicast group.
//
// room / region / priority / ntype are global (CommonConfig). Each slot brings
// its own CCM type and order, which must match the receive CCM configured in
// ArSprout (e.g. WaterTemp 1/13/1).

#pragma once

#include <Arduino.h>
#include <AgriNode.h>
#include "config.h"
#include "sensors.h"

inline bool ccmPublish() {
  if (!g_cfg.common.ccm_enabled) return false;

  bool any = false;
  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    const SlotConfig &sl = g_cfg.slot[i];
    if (!sl.rom[0] || !sl.ccm_type[0] || !g_slotOk[i]) continue;

    char buf[16];
    dtostrf(g_slotTemp[i], 1, 2, buf);
    String xml = agri::ccmEnvelopeOpen();
    xml += agri::ccmDatumNT(sl.ccm_type, g_cfg.common.ccm_ntype,
                            g_cfg.common.ccm_room, g_cfg.common.ccm_region,
                            sl.ccm_order, g_cfg.common.ccm_priority, buf);
    xml += agri::ccmEnvelopeClose();
    any |= agri::ccmSend(xml);
  }
  return any;
}
