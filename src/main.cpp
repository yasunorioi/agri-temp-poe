/*
 * agri-temp-poe — DS18B20 multi-point 1-Wire temperature node on
 *                 M5Stack AtomS3 Lite + Atomic PoE Base (W5500 Ethernet).
 *
 * A PoE derivative of agri-temp-wifi. That node was WiFi only because the
 * ATOM U has no PoE base; the AtomS3 Lite *does* sit on the Atomic PoE base, so
 * this node joins the wired -poe family and is built on agri-node-poe-core
 * (W5500 / MQTT / UECS-CCM / WebUI / mDNS / OTA), exactly like agri-env-poe.
 * The valuable part carried over from agri-temp-wifi is the DS18B20 SLOT MODEL
 * (ROM-address -> slot binding, so probes never silently swap): config.h +
 * sensors.h.
 *
 *   - one slot = one MQTT topic, {value,unit,ts} retained            mqtt_pub.h
 *   - optional UECS-CCM broadcast, one <DATA> per packet             ccm_pub.h
 *   - ROM-address -> slot binding                              config.h/sensors.h
 *   - status LED on G35 (AtomS3 Lite, not the core's G27)            led.h
 *
 * Hardware / wiring: see README.
 */
#include <Arduino.h>
#include <time.h>
#include <ArduinoJson.h>
// Include the core headers individually rather than <AgriNode.h>: that umbrella
// also pulls AgriLED.h, whose begin() hardcodes FastLED.addLeds<WS2812, 27>,
// and GPIO27 is an invalid pin on the ESP32-S3 — its compile-time static_assert
// fails the build even though this node never uses agri::Led (it drives its own
// G35 LED in led.h). AgriOTA.h must precede AgriWebUI.h (the latter uses OTA::).
#include <AgriCommonConfig.h>
#include <AgriNetwork.h>
#include <AgriMQTT.h>
#include <AgriCCM.h>
#include <AgriOTA.h>
#include <AgriWebUI.h>
#include <AgriProvisionAP.h>

#include "config.h"
#include "sensors.h"
#include "led.h"
#include "mqtt_pub.h"
#include "ccm_pub.h"

const char *FW_NAME     = "agri-temp-poe";
const char *FW_VERSION  = "0.1.0";
// GitHub release self-update (core AgriOTA). The tag must be vX.Y.Z and the
// release asset must be named exactly FW_BIN_NAME, or the device finds the tag
// but 404s on the download.
const char *FW_REPO     = "yasunorioi/agri-temp-poe";
const char *FW_BIN_NAME = "agri-temp-poe.bin";

// ===========================================================================
// W5500 pins on the Atomic PoE Base, for the AtomS3 / AtomS3 Lite.
//
// These are NOT the core's default W5500Pins (SCK=22 MISO=23 MOSI=33 CS=19) —
// those are the *classic* ATOM's bottom-header GPIOs, and on the ESP32-S3 they
// don't exist as free pins (26-37 are the embedded flash/PSRAM). The AtomS3
// breaks out G5/G6/G7/G8/G38/G39 on the bottom header and the Atomic PoE Base
// (K139, "Compatible with Atom-Lite/Atom-Matrix/AtomS3/AtomS3-Lite") lands its
// W5500 SPI on four of them:
//
//   SCK=G5  CS=G6  MISO=G7  MOSI=G8   (no reset/interrupt line -> rst=irq=-1)
//
// Confirmed against a known-good m5stack-atoms3 + W5500 config (clk05/cs06/
// miso07/mosi08). Passed via build_flags in platformio.ini so they can be
// changed without editing source; the fallbacks here keep the file
// self-contained.
// ===========================================================================
#ifndef W5500_SCK
#define W5500_SCK  5
#endif
#ifndef W5500_MISO
#define W5500_MISO 7
#endif
#ifndef W5500_MOSI
#define W5500_MOSI 8
#endif
#ifndef W5500_CS
#define W5500_CS   6
#endif

// ---- globals declared extern in the headers --------------------------------
AppConfig g_cfg;
Probe     g_probe[MAX_PROBES];
int       g_probeCount = 0;
float     g_slotTemp[CFG_MAX_SLOTS];
bool      g_slotOk[CFG_MAX_SLOTS];
bool      g_busOk = false;

// ---- small HTML escaping for values in the config/dashboard tables ---------
static String esc(const char *v) {
  String s(v);
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;");
  s.replace("'", "&#39;"); s.replace("\"", "&quot;");
  return s;
}
static String inTxt(const String &name, const String &val, const char *type = "text") {
  return "<input type=" + String(type) + " name='" + name + "' value='" + val + "'>";
}

// <select> of every ROM currently on the bus, plus whatever this slot already
// holds (so a temporarily unplugged probe is not lost by opening the page).
static String romSelect(int i) {
  const char *cur = g_cfg.slot[i].rom;
  String s = "<select name='s" + String(i) + "rom'>";
  s += "<option value=''"; if (!cur[0]) s += " selected"; s += ">(none)</option>";
  bool curListed = false;
  for (int k = 0; k < g_probeCount; k++) {
    const char *r = g_probe[k].rom;
    bool sel = (cur[0] && strcasecmp(cur, r) == 0);
    if (sel) curListed = true;
    s += "<option value='"; s += r; s += "'"; if (sel) s += " selected"; s += ">";
    s += r;
    if (g_probe[k].ok) { s += "  ("; s += String(g_probe[k].temp_c, 1); s += "C)"; }
    int owner = g_probe[k].slot;
    if (owner >= 0 && owner != i) { s += "  [slot "; s += owner; s += "]"; }
    s += "</option>";
  }
  if (cur[0] && !curListed) {
    s += "<option value='"; s += cur; s += "' selected>"; s += cur; s += "  (offline)</option>";
  }
  s += "</select>";
  return s;
}

// ---- Dashboard sensor block (hook) -----------------------------------------
static String renderDashboardSensors() {
  String s; s.reserve(1600);
  s = F("<h3>Temperature</h3><table>"
        "<tr><th>Slot</th><th>Label</th><th>ROM</th><th>Temp</th><th>Topic</th></tr>");
  bool anySlot = false;
  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    const SlotConfig &sl = g_cfg.slot[i];
    if (!sl.rom[0]) continue;
    anySlot = true;
    s += "<tr><td>"; s += i; s += "</td><td>"; s += esc(sl.label);
    s += "</td><td style='font-family:monospace;font-size:12px'>"; s += sl.rom; s += "</td><td>";
    if (g_slotOk[i]) { s += "<span style='color:#3fb950'>"; s += String(g_slotTemp[i], 2); s += " &deg;C</span>"; }
    else             { s += F("<span style='color:#e05252'>-- (no reading)</span>"); }
    s += "</td><td>"; s += esc(sl.topic); s += "</td></tr>";
  }
  if (!anySlot) s += F("<tr><td colspan=5 style='color:#e8a33d'>No probe bound yet — see Config</td></tr>");
  s += F("</table>");

  // Probes physically on the bus that no slot claims: the thing you actually
  // want to see right after adding or swapping a sensor.
  String un; int nun = 0;
  for (int i = 0; i < g_probeCount; i++) {
    if (g_probe[i].slot >= 0) continue;
    nun++;
    un += "<tr><td style='font-family:monospace;font-size:12px'>"; un += g_probe[i].rom; un += "</td><td>";
    if (g_probe[i].ok) { un += String(g_probe[i].temp_c, 2); un += " &deg;C"; }
    else               un += F("<span style='color:#e05252'>--</span>");
    un += "</td></tr>";
  }
  if (nun) {
    s += F("<h3 style='color:#e8a33d'>Unassigned probes on the bus</h3>"
           "<table><tr><th>ROM</th><th>Temp</th></tr>");
    s += un;
    s += F("</table><p style='color:#e8a33d'>Bind these to a slot in Config to publish them.</p>");
  }
  s += "<p>Bus: G"; s += g_cfg.ow_pin; s += ", ";
  s += g_probeCount; s += F(" probe(s) found");
  if (!g_busOk) s += F(" <span style='color:#e05252'>— bus empty: check the Grove connection</span>");
  s += F("</p>");
  return s;
}

// ---- Config sensor rows (hook): appended inside the core Config <table> ----
static String renderConfigSensorRows() {
  String s; s.reserve(2000);
  auto hdr = [&](const char *t) {
    s += F("<tr><th colspan=2 style='background:#1a1a1f;color:#9ad;text-align:left;padding:8px'>");
    s += t; s += F("</th></tr>");
  };
  auto row = [&](const char *label, const String &input) {
    s += "<tr><th>"; s += label; s += "</th><td>"; s += input; s += "</td></tr>";
  };

  hdr("1-Wire bus");
  row("DATA pin (GPIO)",        inTxt("owpin", String(g_cfg.ow_pin), "number"));
  row("Resolution (9-12 bit)",  inTxt("owres", String(g_cfg.resolution), "number"));
  row("Measure interval (s)",   inTxt("msint", String(g_cfg.meas_interval_s), "number"));

  hdr("Slots — bind a probe (ROM) to a slot");
  s += F("<tr><td colspan=2>"
         "<p style='color:#999;margin:4px 0'>Empty ROM = slot inactive. "
         "Empty Topic = not published. Empty CCM識別子 = no CCM for that slot. "
         "room / region / priority / ノード種別 are shared (set above under UECS-CCM).</p>"
         "<table><tr><th>#</th><th>ROM</th><th>Label</th><th>MQTT topic</th>"
         "<th>CCM識別子</th><th>order</th><th>offset &deg;C</th></tr>");
  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    const SlotConfig &sl = g_cfg.slot[i];
    String p = "s" + String(i);
    s += "<tr><td>"; s += i; s += "</td><td>"; s += romSelect(i); s += "</td>";
    s += "<td>" + inTxt(p + "lab", esc(sl.label))    + "</td>";
    s += "<td>" + inTxt(p + "top", esc(sl.topic))    + "</td>";
    s += "<td>" + inTxt(p + "typ", esc(sl.ccm_type)) + "</td>";
    s += "<td>" + inTxt(p + "or",  String(sl.ccm_order), "number") + "</td>";
    s += "<td>" + inTxt(p + "off", String(sl.offset_c, 2)) + "</td></tr>";
  }
  s += F("</table></td></tr>");
  return s;
}

// ---- Apply the sensor rows (hook) ------------------------------------------
static void applyConfigSensorForm(const String &body) {
  int newpin = agri::parseFormInt(body, "owpin", g_cfg.ow_pin);
  int res    = agri::parseFormInt(body, "owres", g_cfg.resolution);
  g_cfg.resolution      = (uint8_t)constrain(res, 9, 12);
  int msi = agri::parseFormInt(body, "msint", g_cfg.meas_interval_s);
  g_cfg.meas_interval_s = (uint16_t)max(1, msi);

  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    SlotConfig &sl = g_cfg.slot[i];
    String p = "s" + String(i);
    agri::parseFormStr(body, (p + "rom").c_str(), sl.rom,      sizeof(sl.rom));
    agri::parseFormStr(body, (p + "lab").c_str(), sl.label,    sizeof(sl.label));
    agri::parseFormStr(body, (p + "top").c_str(), sl.topic,    sizeof(sl.topic));
    agri::parseFormStr(body, (p + "typ").c_str(), sl.ccm_type, sizeof(sl.ccm_type));
    sl.ccm_order = (int16_t)agri::parseFormInt(body, (p + "or").c_str(), sl.ccm_order);
    char off[16]; dtostrf(sl.offset_c, 1, 2, off);
    agri::parseFormStr(body, (p + "off").c_str(), off, sizeof(off));
    sl.offset_c = atof(off);
  }

  if ((uint8_t)newpin != g_cfg.ow_pin) {
    sensorsRebind((uint8_t)newpin);        // re-inits the bus on the new pin + rescans
  } else {
    if (owDs()) owDs()->setResolution(g_cfg.resolution);
    sensorsScan();                         // re-resolve ROM -> slot bindings now
  }
  agri::MQTT::mqtt.disconnect();           // reconnect with the new host/topics
}

// ---- extra /api/status fields (hook) ---------------------------------------
static void addStatusFields(JsonObject root) {
  root["ow_pin"]      = g_cfg.ow_pin;
  root["resolution"]  = g_cfg.resolution;
  root["bus_ok"]      = g_busOk;
  root["probe_count"] = g_probeCount;

  JsonArray ps = root["probes"].to<JsonArray>();
  for (int i = 0; i < g_probeCount; i++) {
    JsonObject o = ps.add<JsonObject>();
    o["rom"]  = g_probe[i].rom;
    o["slot"] = g_probe[i].slot;
    if (g_probe[i].ok) o["temp_c"] = g_probe[i].temp_c; else o["temp_c"] = nullptr;
  }
  JsonArray ss = root["slots"].to<JsonArray>();
  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    if (!g_cfg.slot[i].rom[0]) continue;
    JsonObject o = ss.add<JsonObject>();
    o["slot"]  = i;
    o["label"] = g_cfg.slot[i].label;
    o["topic"] = g_cfg.slot[i].topic;
    if (g_slotOk[i]) o["temp_c"] = g_slotTemp[i]; else o["temp_c"] = nullptr;
  }
}

// ---- setup -----------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n=== %s v%s ===\n", FW_NAME, FW_VERSION);

  appled::begin();
  for (int i = 0; i < CFG_MAX_SLOTS; i++) { g_slotTemp[i] = NAN; g_slotOk[i] = false; }

  loadConfig();
  Serial.printf("[CFG] node=%s mqtt_host=%s prefix=%s\n",
                g_cfg.common.node_id,
                g_cfg.common.mqtt_host[0] ? g_cfg.common.mqtt_host : "(unset)",
                g_cfg.common.mqtt_topic_prefix);

  sensorsBegin();

  agri::W5500Pins pins;
  pins.sck  = W5500_SCK;
  pins.miso = W5500_MISO;
  pins.mosi = W5500_MOSI;
  pins.cs   = W5500_CS;
  agri::Network::begin(g_cfg.common.hostname, pins);
  agri::Network::waitForLease();

  configTime(0, 0, "ntp.nict.jp", "pool.ntp.org");   // SNTP for the MQTT ts field

  agri::MQTT::begin();
  agri::ccmBegin();

  agri::WebHooks hooks;
  hooks.nodeTitle              = [](){ return FW_NAME; };
  hooks.renderDashboardSensors = renderDashboardSensors;
  hooks.renderConfigSensorRows = renderConfigSensorRows;
  hooks.applyConfigSensorForm  = applyConfigSensorForm;
  hooks.addStatusFields        = addStatusFields;
  hooks.saveConfig             = [](){ saveConfig(); };
  agri::WebUI::begin(g_cfg.common, hooks, FW_NAME, FW_VERSION);

  agri::mdnsBegin(g_cfg.common.hostname);
  agri::otaBegin(g_cfg.common.hostname);

  agri::OTA::begin(FW_REPO, FW_BIN_NAME, FW_VERSION);
  agri::OTA::checkLatest();     // once at boot; poll() re-checks daily

  Serial.printf("[BOOT] %s %s  probes=%d  mqtt=%s  ccm=%s\n",
                FW_NAME, FW_VERSION, g_probeCount,
                g_cfg.common.mqtt_host[0] ? g_cfg.common.mqtt_host : "(none)",
                g_cfg.common.ccm_enabled ? "on" : "off");
}

// ---- loop ------------------------------------------------------------------
// Conversion is asynchronous, so a measurement is two timed steps:
//   REQUEST -> (CONVERT_MS) -> COLLECT -> (meas_interval_s) -> REQUEST ...
enum MeasState { MEAS_IDLE, MEAS_CONVERTING };
static MeasState measState = MEAS_IDLE;
static uint32_t  lastMeasMs = 0, convertStartMs = 0;
static uint32_t  lastMqttMs = 0, lastCcmMs = 0, lastMqttTryMs = 0, lastScanMs = 0;

void loop() {
  agri::otaHandle();
  agri::OTA::poll();     // daily re-check; flashes only when /api/update armed it
  agri::WebUI::handle(agri::Network::link_up, agri::Network::have_lease);
  // No DHCP lease for a grace period (cable out / no LAN / just-unboxed) -> raise
  // a WPA2 SoftAP (SSID = hostname) serving this same WebUI so the node can be
  // configured wirelessly. Torn down automatically once Ethernet gets a lease.
  agri::ProvisionAP::poll(agri::Network::have_lease, g_cfg.common.hostname);

  uint32_t now = millis();

  // MQTT keepalive / reconnect (non-blocking, retry every 5 s)
  if (agri::networkUp() && agri::MQTT::hasHost(g_cfg.common)) {
    if (!agri::MQTT::connected()) {
      if (now - lastMqttTryMs > 5000) { lastMqttTryMs = now; agri::MQTT::reconnect(g_cfg.common); }
    } else {
      agri::MQTT::loop();
    }
  }

  // Re-enumerate every 60 s so a probe added or replaced in the field shows up
  // without a reboot. Cheap: a bus search, not a conversion.
  if (now - lastScanMs > 60000) { lastScanMs = now; sensorsScan(); }

  switch (measState) {
    case MEAS_IDLE:
      if (now - lastMeasMs < (uint32_t)g_cfg.meas_interval_s * 1000) break;
      lastMeasMs = now;
      sensorsRequest();
      convertStartMs = now;
      measState = MEAS_CONVERTING;
      break;

    case MEAS_CONVERTING:
      if (now - convertStartMs < (uint32_t)CONVERT_MS) break;
      sensorsCollect();
      measState = MEAS_IDLE;

      for (int i = 0; i < CFG_MAX_SLOTS; i++) {
        if (g_cfg.slot[i].rom[0] && g_slotOk[i]) {
          Serial.printf("[TEMP] slot%d %-8s %.2f C\n", i, g_cfg.slot[i].label, g_slotTemp[i]);
        }
      }

      if (agri::MQTT::connected() &&
          now - lastMqttMs >= (uint32_t)g_cfg.common.mqtt_interval_s * 1000) {
        lastMqttMs = now;
        if (mqttPublishAll()) appled::flashPublish();
      }
      if (g_cfg.common.ccm_enabled && agri::networkUp() &&
          now - lastCcmMs >= (uint32_t)g_cfg.common.ccm_interval_s * 1000) {
        lastCcmMs = now;
        if (ccmPublish()) appled::flashPublish();
      }
      break;
  }

  // Status LED
  appled::State led;
  if (!agri::networkUp())                                                led = appled::NO_LINK;
  else if (!g_busOk)                                                     led = appled::NO_SENSOR;
  else if (agri::MQTT::hasHost(g_cfg.common) && !agri::MQTT::connected()) led = appled::NO_MQTT;
  else                                                                   led = appled::OK;
  appled::set(led);

  delay(20);
}
