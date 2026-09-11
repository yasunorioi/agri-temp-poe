# agri-temp-poe

[🇯🇵 日本語](README_ja.md) · **English**

A multi-point temperature node built from an M5Stack **AtomS3 Lite** + **Atomic PoE Base (K139, W5500)** + **DS18B20 × N** (1-Wire multi-drop). The **PoE member** of the `agri-*` family. Default target is house 2 water temperature (`WaterTemp`).

A **PoE derivative of `agri-temp-wifi`**. That node was WiFi only because the ATOM U cannot sit on a PoE base; the **AtomS3 Lite does sit on the Atomic PoE base**, so this node stands on **`agri-node-poe-core`** like the other `-poe` nodes (env / rain / flow / solar). What carried over from `agri-temp-wifi` is its core asset — the **DS18B20 slot model** (`config.h` / `sensors.h`).

| | agri-temp-wifi | **agri-temp-poe** |
|---|---|---|
| Base | own code (`src/*.h`, WiFi) | **`agri-node-poe-core`** (W5500/ETH) |
| MCU | ATOM U (ESP32-PICO) | **AtomS3 Lite (ESP32-S3)** |
| Link | WiFi (WiFiManager) | **PoE / W5500 Ethernet** |
| Sensor | bare DS18B20 ×N (external 4.7k required) | **Grove DS18B20 unit (built-in pull-up)** |
| LED | on-board (G27) | on-board **G35** (core's G27 unused → `led.h`) |
| Self-update | own `self_update.h` | core's `AgriOTA` (3.x, so core is usable directly) |

---

## Wiring

Just plug the Grove unit (Switch Science 10979 / DS18B20 waterproof probe, 2 m cable, **built-in pull-up**) into the AtomS3 Grove port. **No external 4.7k needed** — the biggest difference from agri-temp-wifi.

```
AtomS3 Grove port (HY2.0-4P)        Grove DS18B20 unit
  G1 (white)  ───────────────────── DATA   (pull-up is built in)
  G2 (yellow/SIG) ── (unused)
  5V (red)    ───────────────────── VDD
  GND (black) ───────────────────── GND
```

- **DATA = G1**. On this unit DATA comes out on the white wire = G1, verified on hardware — the yellow/SIG = G2 the silkscreen suggests detected nothing. Editable in `/config`; the bus re-inits with no reboot.
- Operating voltage 3.0–5.5 V. The unit may be powered at 5 V (the built-in pull-up is on the Grove logic-voltage side, so no overvoltage reaches G1).
- If you need more probes, multi-drop them on the same bus via a Grove splitter (avoid star wiring).

### W5500 (Atomic PoE Base) pins

`agri-node-poe-core`'s W5500 defaults (SCK=22 / MISO=23 / MOSI=33 / CS=19) are the **classic ATOM's bottom-header pins** and don't work on the ESP32-S3 (26–37 are the embedded flash/PSRAM). The Atomic PoE Base (K139, "Compatible with … AtomS3/AtomS3-Lite") lands its W5500 SPI on AtomS3 bottom-header pins **G5/G6/G7/G8**:

```
SCK = G5    CS = G6    MISO = G7    MOSI = G8    (no reset/interrupt = -1)
```

They are passed via the `-DW5500_*` build flags in `platformio.ini`, matching a known-good m5stack-atoms3 + W5500 config (clk05/cs06/miso07/mosi08). Edit here if you change base/wiring. Unlike the 1-Wire pin, SPI has no auto-probe fallback, so **failing to get DHCP → suspect the pins or wiring**.

---

## Slot model (same as `agri-temp-wifi`)

1-Wire enumerates in ROM-address order, **not the order you wired the probes**. An index-based mapping would silently swap every series the moment one sensor is replaced. So the config binds a **ROM address → slot**, and the **slot** owns the MQTT topic / UECS type / calibration offset.

- Empty ROM = slot inactive / empty Topic = not published to MQTT / empty CCM識別子 (CCM id) = not sent as CCM
- On first boot, if no slot holds a ROM yet, probes are **auto-assigned in bus order** and saved to NVS
- The Config ROM field is a `<select>` of the ROMs on the bus (with current temperature). **A ROM belonging to no slot shows on the Dashboard as an "Unassigned probe"** — so you notice immediately when you add a sensor
- Confirm identity by **gripping one probe at a time and watching which Dashboard reading rises**, then assign its label/topic

> Difference from `agri-temp-wifi`: room / region / priority / node type are **shared** (`CommonConfig`, the UECS-CCM section of `/config`). A slot only carries its own CCM **type and order**. All probes are assumed to report into one house/region — the normal shape for a single sensor node.

---

## MQTT

Per slot, **one measurand = one topic**, `retain`ed. Each slot holds its topic as a **full path** (rather than env-poe's `<prefix>/sensor/<Type>`), because multi-point temperature often wants to publish to unrelated topics.

```
agriha/2/sensor/WaterTemp      {"value":21.44,"unit":"C","ts":1788334103}
agriha/2/sensor/WaterTemp/2    {"value":19.80,"unit":"C","ts":1788334103}
agriha/2/sys/temp_poe_01/online   1 / 0  (LWT, retain — added by core)
```

`ts` is the real epoch after SNTP sync, or `0` if not yet synced.

## UECS-CCM (optional, OFF by default)

Per slot, **one `<DATA>` per packet**, sent both broadcast and multicast (ArSprout only takes the last DATA and only receives on 255.255.255.255 — see `AgriCCM.h`).

> **⚠️ Enabling CCM alone does not make data reach agriha.** yasu-hp's `ccm-mqtt-bridge` decides the house from a `sender_override` per source IP and drops unregistered IPs. The default region=13 is provisional — always reconcile before turning it ON. (**Native MQTT publish is self-contained, so leaving CCM OFF is fine.**)

---

## Build / flash

Put `pio` on your PATH and the commands are **identical on Windows / Linux** (`platformio.ini` is OS-independent; `upload_port` unset = auto-detect):

```bash
pio run -e m5atoms3-poe            # build
pio run -e m5atoms3-poe -t upload  # USB flash (first time only); port auto-detected
```

- **Same pioarduino fork as `agri-node-poe-core`** (arduino-esp32 3.x, needed for W5500's `ETH.begin(ETH_PHY_W5500, …)`).
- The AtomS3 Lite has no USB-UART chip; Serial is the **ESP32-S3 native USB CDC** (VID 0x303a). Hence `-DARDUINO_USB_CDC_ON_BOOT=1` (classic ATOM nodes set it to 0).
- USB flash is first-time only. After that, OTA over Ethernet (`curl` is `curl.exe` on Windows):
  ```bash
  curl -F firmware=@.pio/build/m5atoms3-poe/firmware.bin http://agri-temp-poe-01.local/api/ota
  ```

> 🛠 **Build environment (shared Windows / Linux) and Linux first-time setup (udev / pipx / not sharing `~/.platformio`)** → see the fleet-wide primary reference:
> [agri-node-poe-core/docs/cross-platform-build.md](https://github.com/yasunorioi/agri-node-poe-core/blob/main/docs/cross-platform-build.md)

Measured: RAM 11.1% / Flash 29.8% (994 KB, pioarduino fork = arduino-esp32 3.x).

## First-time setup

1. Flash over USB → power via PoE (PoE hub / injector)
2. Get an IP via DHCP (LED: blue=boot → red=no link / no lease → green=OK)
   - **Stays red / never turns green** → suspect the W5500 wiring (pin section above)
3. Open `http://agri-temp-poe-01.local/`
4. In `/config` set the MQTT Host (`yasu-hp.local`) and the slots
5. Temperature appears on the Dashboard → confirm `agriha/2/sensor/WaterTemp` on the broker

> **When bringing up multiple units (4):** the default hostname / node_id are the same on every unit (`agri-temp-poe-01` / `temp_poe_01`). Dropping them onto the same LAN at once causes an mDNS name clash plus an MQTT client-id clash (same node_id → the broker kicks one off). **Flash one at a time and give each a unique hostname and node_id in `/config`** (e.g. `-01`…`-04`) before connecting the next. The house assignment (topic prefix) can stay at the default house 2.

### When you can't get it on the LAN (SoftAP fallback)

If there is no DHCP lease (cable unplugged / no DHCP on the LAN / just out of the box with an unknown IP) for ~15 s after boot, a **provisioning SoftAP** comes up automatically (core `AgriProvisionAP`). It is torn down once Ethernet gets a lease.

1. Join the SSID **`agri-temp-poe-01`** (= hostname) from a phone/PC
   - Password: **`agrinode`** (WPA2, shared across the fleet; override at build time with `-DAGRI_AP_PASSWORD=\"...\"`)
2. The captive portal opens `/config` automatically (if not, browse to `http://192.168.4.1/`)
3. Set the hostname (= mDNS name) / MQTT Host etc. and Save
4. Reconnect Ethernet and the SoftAP stops automatically → go to `http://<new hostname>.local/`

> This just serves the same `AgriWebUI` over the AP, so the settings are exactly those of `/config`. W5500 (SPI) and WiFi (radio) don't conflict and can run simultaneously.

### Status LED (G35)

Same color scheme as `agri-node-poe-core` (implementation in `led.h`, only the pin differs = G35):
blue=boot / red=no link / **purple=empty bus (no DS18B20 detected)** / orange=MQTT not connected / green=OK / white=publish blink

## API (provided by core `AgriWebUI`)

| | |
|---|---|
| `GET /` | Dashboard (refreshes `/api/status` + `/api/dashboard` every 3 s) |
| `GET /config` · `POST /config` | Settings form (common + slots) |
| `GET /api/status` | common (fw/ip/link/mqtt/ccm/uptime/ota) + `ow_pin`/`bus_ok`/`probe_count`/`probes[]`/`slots[]` |
| `GET /api/config` | common config JSON (slots are under `/api/status`) |
| `POST /api/ota` | multipart firmware update |
| `GET /ota` / `POST /api/check` / `POST /api/update` | GitHub Release self-update (semi-automatic) |
| `POST /api/reboot` | Reboot over the network (Reboot button on `/ota`). Settings survive (NVS) |

> Changing the DATA pin re-inits the bus **without a reboot** (`sensorsRebind`). Bus re-enumeration runs every 60 s and on config save, so the "Rescan" button from the old WiFi node is dropped.

## Self-update (GitHub Release)

Uses core's `AgriOTA` directly (the WiFi node's own `self_update.h` is not needed). Semi-automatic = checks for a Release at boot and every 24 h; if a newer one exists, a banner + "Update" button appear on the Dashboard. Pressing it schedules via `/api/update` and the next `poll()` flashes and reboots.

```bash
pio run -e m5atoms3-poe
# The asset name is fixed via gh's path#name notation (no Copy-Item; same on Win/Linux)
gh release create v0.1.0 ".pio/build/m5atoms3-poe/firmware.bin#agri-temp-poe.bin" \
  --title v0.1.0 --notes "..."
```

- Tag = `v` + `FW_VERSION` (`main.cpp`) / asset name = exactly `agri-temp-poe.bin` (`#agri-temp-poe.bin` guarantees this)

## TODO

- **Build check on hardware** (this repo is unbuilt; assumes the family-wide pioarduino fork).
- Give each of the 4 units a **unique hostname / node_id** (see the first-time-setup note).
- Map ROM ↔ physical probe with real probes (grip one at a time and check the Dashboard).
- If using CCM, add one `sender_override` line to yasu-hp's bridge.
