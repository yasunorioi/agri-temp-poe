// config.h — agri-temp-poe NVS-backed config ("tempp-cfg").
//
// PoE member of the agri-* family: AtomS3 Lite + M5 Atom PoE Base (W5500).
// Network / MQTT / UECS-CCM envelope come from agri-node-poe-core's
// CommonConfig; this struct adds the 1-Wire bus settings and the DS18B20
// SLOT MODEL carried over from agri-temp-wifi.
//
// SLOT MODEL (unchanged from agri-temp-wifi)
// ------------------------------------------
// A 1-Wire bus carries N DS18B20s, each with a unique 64-bit ROM address. The
// bus enumerates in ROM order, which is arbitrary and NOT the order you wired
// them in — so an index-based mapping would silently swap probes whenever one
// is replaced. Instead the config binds a ROM address to a *slot*, and the slot
// owns the MQTT topic / UECS type / calibration. Swap a probe, rebind one field
// in /config, and every downstream series keeps its meaning.
//
// A slot with an empty rom is inactive. A slot with an empty topic is not
// published to MQTT; a slot with an empty ccm_type is not sent as CCM. That
// mirrors the family convention (agri-env-poe: empty CCM識別子 = datum disabled).
//
// DIFFERENCE FROM agri-temp-wifi: room / region / priority / ntype are global
// (they live in CommonConfig, per the -poe family convention), so a slot only
// carries its own CCM *type* and *order*. All probes on this node therefore
// report into one house/region — the common case for a single sensor node.

#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <AgriCommonConfig.h>

static const int CFG_MAX_SLOTS = 8;

struct SlotConfig {
  char    rom[17];        // 16 uppercase hex chars, "" = unbound/inactive
  char    label[16];      // human label shown in the WebUI (e.g. "供給")
  char    topic[64];      // full MQTT topic, "" = do not publish
  char    ccm_type[24];   // UECS type name (CCM識別子), "" = no CCM for this slot
  int16_t ccm_order;      // per-slot UECS order (room/region/priority = common)
  float   offset_c;       // per-probe calibration offset added to the reading
};

struct AppConfig {
  agri::CommonConfig common;

  // 1-Wire bus
  uint8_t  ow_pin;                  // DATA pin (default G2 = AtomS3 Grove yellow)
  uint8_t  resolution;              // 9..12 bits (12 = 0.0625 C, 750 ms)
  uint16_t meas_interval_s;         // bus poll cadence

  SlotConfig slot[CFG_MAX_SLOTS];
};

extern AppConfig g_cfg;

// NVS key for slot i, e.g. slotKey("rom", 3) -> "s3rom". Keys must stay <= 15.
inline const char *slotKey(const char *field, int i) {
  static char buf[16];
  snprintf(buf, sizeof(buf), "s%d%s", i, field);
  return buf;
}

inline void setDefaults() {
  agri::commonDefaults(g_cfg.common,
                       "temp_poe_01", "agri-temp-poe-01",
                       /*mqtt prefix = house scope*/ "agriha/2",
                       /*default_ccm_region = 別棟 ArSprout region*/ 13);

  // Grove yellow (SIG) on AtomS3 Lite = G2 (white = G1). The Grove DS18B20
  // unit has a built-in pull-up, so no external 4.7k is needed — unlike the
  // bare probes on agri-temp-wifi. See README "配線".
  g_cfg.ow_pin          = 2;
  g_cfg.resolution      = 12;
  g_cfg.meas_interval_s = 10;

  // Default routing mirrors agri-temp-wifi: house2 water temperature. Slot 0
  // owns the bare type; the rest take the "/N" instance suffix already used on
  // this broker for repeated types (agriha/2/actuator/Relay/2, etc.). Every
  // field is editable in /config — retarget freely (soil, air, tank, …).
  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    SlotConfig &s = g_cfg.slot[i];
    s.rom[0] = '\0';
    snprintf(s.label, sizeof(s.label), "probe%d", i + 1);
    if (i == 0) strlcpy(s.topic, "agriha/2/sensor/WaterTemp", sizeof(s.topic));
    else        snprintf(s.topic, sizeof(s.topic), "agriha/2/sensor/WaterTemp/%d", i + 1);
    strlcpy(s.ccm_type, "WaterTemp", sizeof(s.ccm_type));
    s.ccm_order = i + 1;
    s.offset_c  = 0.0f;
  }
}

inline void loadConfig() {
  setDefaults();
  Preferences p;
  if (!p.begin("tempp-cfg", true)) return;
  agri::commonLoad(g_cfg.common, p);

  g_cfg.ow_pin          = p.getUChar ("ow_pin", g_cfg.ow_pin);
  g_cfg.resolution      = p.getUChar ("ow_res", g_cfg.resolution);
  g_cfg.meas_interval_s = p.getUShort("ms_int", g_cfg.meas_interval_s);

  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    SlotConfig &sl = g_cfg.slot[i];
    // String-default overload keeps the setDefaults() value when the key is
    // absent (a fresh NVS) instead of wiping it to "".
    String s;
    s = p.getString(slotKey("rom", i), sl.rom);      strlcpy(sl.rom,      s.c_str(), sizeof(sl.rom));
    s = p.getString(slotKey("lab", i), sl.label);    strlcpy(sl.label,    s.c_str(), sizeof(sl.label));
    s = p.getString(slotKey("top", i), sl.topic);    strlcpy(sl.topic,    s.c_str(), sizeof(sl.topic));
    s = p.getString(slotKey("typ", i), sl.ccm_type); strlcpy(sl.ccm_type, s.c_str(), sizeof(sl.ccm_type));
    sl.ccm_order = p.getShort(slotKey("or",  i), sl.ccm_order);
    sl.offset_c  = p.getFloat(slotKey("off", i), sl.offset_c);
  }
  p.end();
}

inline bool saveConfig() {
  Preferences p;
  if (!p.begin("tempp-cfg", false)) return false;
  agri::commonSave(g_cfg.common, p);
  p.putUChar ("ow_pin", g_cfg.ow_pin);
  p.putUChar ("ow_res", g_cfg.resolution);
  p.putUShort("ms_int", g_cfg.meas_interval_s);
  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    const SlotConfig &sl = g_cfg.slot[i];
    p.putString(slotKey("rom", i), sl.rom);
    p.putString(slotKey("lab", i), sl.label);
    p.putString(slotKey("top", i), sl.topic);
    p.putString(slotKey("typ", i), sl.ccm_type);
    p.putShort (slotKey("or",  i), sl.ccm_order);
    p.putFloat (slotKey("off", i), sl.offset_c);
  }
  p.end();
  return true;
}
