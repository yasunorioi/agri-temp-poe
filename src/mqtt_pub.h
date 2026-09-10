// mqtt_pub.h — one slot, one topic, one value.
//
// Uses the core MQTT client (agri::MQTT), but keeps agri-temp-wifi's slot model
// where each slot stores its FULL topic string (e.g. "agriha/2/sensor/WaterTemp")
// rather than prefix+type. That's more flexible than the env-poe "<prefix>/
// sensor/<Type>" scheme — a multi-probe temperature node often wants probes on
// unrelated topics — and it is what the WebUI slot table edits directly.
//
// Payload is the canonical agriha per-type form (mqtt-topics.md §0.2/§0.4):
//     {"value":21.44,"unit":"C","ts":1788334103}
// retained, one physical quantity per topic. `ts` is UTC epoch once SNTP has
// synced, else 0 (consumers treat 0 as "no clock").

#pragma once

#include <Arduino.h>
#include <time.h>
#include <AgriMQTT.h>   // not <AgriNode.h>: avoid AgriLED.h's G27 addLeds (invalid on S3)
#include "config.h"
#include "sensors.h"

inline long mqttNow() {
  time_t t = time(nullptr);
  return (t > 1700000000) ? (long)t : 0;
}

inline bool mqttPublishSlot(int i) {
  const SlotConfig &sl = g_cfg.slot[i];
  if (!agri::MQTT::connected() || !sl.rom[0] || !sl.topic[0] || !g_slotOk[i]) return false;

  char payload[96];
  int n = snprintf(payload, sizeof(payload),
                   "{\"value\":%.2f,\"unit\":\"C\",\"ts\":%ld}",
                   g_slotTemp[i], mqttNow());
  bool ok = agri::MQTT::mqtt.publish(sl.topic, (const uint8_t *)payload,
                                     (unsigned)n, /*retain=*/true);
  Serial.printf("[MQTT] slot%d %s %s %.2fC\n", i, sl.topic, ok ? "OK" : "FAIL", g_slotTemp[i]);
  return ok;
}

inline bool mqttPublishAll() {
  if (!agri::MQTT::hasHost(g_cfg.common) || !agri::MQTT::connected()) return false;
  bool any = false;
  for (int i = 0; i < CFG_MAX_SLOTS; i++) any |= mqttPublishSlot(i);
  return any;
}
