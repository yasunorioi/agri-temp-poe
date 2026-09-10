# agri-temp-poe

M5Stack **AtomS3 Lite** ＋ **Atomic PoE Base（K139, W5500）** ＋ **DS18B20 × N**（1-Wire マルチドロップ）
の多点温度ノード。`agri-*` ファミリーの **PoE 機**。既定は house2 の水温（`WaterTemp`）。

`agri-temp-wifi` の **PoE 派生**。あちらが WiFi だったのは ATOM U に PoE ベースが
履けないからで、**AtomS3 Lite は Atomic PoE ベースに載る**ため、この機は他の
`-poe` ノード（env / rain / flow / solar）と同じく **`agri-node-poe-core`** の上に立つ。
持ち込んだ資産は `agri-temp-wifi` の核心 = **DS18B20 のスロットモデル**
（`config.h` / `sensors.h`）。

| | agri-temp-wifi | **agri-temp-poe** |
|---|---|---|
| 基盤 | 自前 (`src/*.h`, WiFi) | **`agri-node-poe-core`** (W5500/ETH) |
| MCU | ATOM U (ESP32-PICO) | **AtomS3 Lite (ESP32-S3)** |
| 回線 | WiFi (WiFiManager) | **PoE / W5500 Ethernet** |
| センサ | 裸 DS18B20 ×N（外付 4.7k 必須） | **Grove DS18B20 ユニット（プルアップ内蔵）** |
| LED | 内蔵 (G27) | 内蔵 **G35**（core の G27 は使わない → `led.h`） |
| セルフ更新 | 自前 `self_update.h` | core の `AgriOTA`（3.x なので core を直接使える） |

---

## 配線

Grove ユニット（Switch Science 10979 / DS18B20 防水プローブ・2m・**プルアップ内蔵**）を
AtomS3 の Grove ポートに挿すだけ。**外付け 4.7k は不要**（agri-temp-wifi との最大の差）。

```
AtomS3 Grove ポート (HY2.0-4P)      Grove DS18B20 ユニット
  G2 (黄/SIG) ───────────────────── DATA   (プルアップは内蔵)
  G1 (白)     ── (未使用)
  5V (赤)     ───────────────────── VDD
  GND(黒)     ───────────────────── GND
```

- **DATA = G2**（AtomS3 の Grove 黄 = SIG。`/config` で変更可、再起動不要でバス再構築）
- 動作電圧 3.0–5.5V。ユニットは 5V 給電で使ってよい（内蔵プルアップは Grove の
  ロジック電圧側に付いており、G2 に過電圧は乗らない設計）。
- 追加プローブが要るなら Grove 分岐で同一バスにマルチドロップ可（スター配線は避ける）。

### W5500（Atomic PoE Base）のピン

`agri-node-poe-core` の W5500 デフォルト（SCK=22 / MISO=23 / MOSI=33 / CS=19）は
**旧 ATOM の底面ピン**で、ESP32-S3 では使えない（26–37 は内蔵フラッシュ/PSRAM）。
Atomic PoE Base（K139、"Compatible with … AtomS3/AtomS3-Lite"）は AtomS3 の底面ヘッダ
**G5/G6/G7/G8** に W5500 SPI を落とす:

```
SCK = G5    CS = G6    MISO = G7    MOSI = G8    (reset/interrupt 無し = -1)
```

`platformio.ini` の `-DW5500_*` build フラグで渡している。実績のある m5stack-atoms3 +
W5500 構成（clk05/cs06/miso07/mosi08）と一致。別のベース/配線に変えるならここを直す。
1-Wire ピンと違い SPI には自動探索フォールバックが無いので、**DHCP を取れない＝ピンか
配線を疑う**。

---

## スロットモデル（`agri-temp-wifi` と同じ）

1-Wire は列挙順＝ROM アドレス順で、**配線した順ではない**。添字対応だとセンサーを
1本替えただけで全系列の意味が入れ替わる。そこで **ROM アドレス → スロット**を config で
束縛し、**スロット**が MQTT トピック / UECS 型 / 校正オフセットを持つ。

- ROM 空 = スロット無効 / Topic 空 = MQTT に出さない / CCM識別子 空 = CCM を出さない
- 初回、どのスロットにも ROM が無ければ**バス順に自動割当**して NVS 保存
- Config の ROM 欄はバス上の ROM の `<select>`（現在温度付き）。**どのスロットにも属さない
  ROM は Dashboard に「Unassigned probes」**として出る＝センサーを足せばすぐ気づく
- **1本ずつ手で握って Dashboard のどれが上がるか**で実体を確認してからラベル/トピックを付ける

> `agri-temp-wifi` との差: room / region / priority / ノード種別は **共通**
> （`CommonConfig`、`/config` の UECS-CCM 欄）。スロットが持つ CCM 項目は **型と order のみ**。
> 全プローブが 1 ハウス/region に入る前提（単一センサノードの通常形）。

---

## MQTT

スロットごとに **1物理量1トピック**、`retain`。トピックはスロットが**フルパスで**保持する
（env-poe の `<prefix>/sensor/<Type>` ではなく、温度多点は無関係なトピックに振りたいことが
多いのでフル指定）。

```
agriha/2/sensor/WaterTemp      {"value":21.44,"unit":"C","ts":1788334103}
agriha/2/sensor/WaterTemp/2    {"value":19.80,"unit":"C","ts":1788334103}
agriha/2/sys/temp_poe_01/online   1 / 0  (LWT, retain — core が付与)
```

`ts` は SNTP 同期後の実 epoch、未同期なら `0`。

## UECS-CCM（任意・既定 OFF）

スロットごとに **1パケット1 `<DATA>`**、ブロードキャスト + マルチキャスト両送出
（ArSprout は複数 DATA の最後しか取らず 255.255.255.255 でしか受けない — `AgriCCM.h` 参照）。

> **⚠️ CCM を有効にしても、それだけでは agriha には出ない。** yasu-hp の
> `ccm-mqtt-bridge` は送信元 IP ごとの `sender_override` でハウスを決め、未登録 IP は
> drop する。既定 region=13 は暫定。ON にする前に必ず突き合わせる
> （**MQTT ネイティブ publish で完結するので CCM は基本 OFF のままでよい**）。

---

## ビルド / 書き込み

`pio` を PATH に通せば **Windows / Linux 同一コマンド**（`platformio.ini` は OS 非依存、
`upload_port` 未指定＝自動検出）:

```bash
pio run -e m5atoms3-poe            # ビルド
pio run -e m5atoms3-poe -t upload  # USB 書き込み(初回のみ)。ポートは自動検出
```

- **`agri-node-poe-core` と同じ pioarduino fork**（arduino-esp32 3.x。W5500 の
  `ETH.begin(ETH_PHY_W5500, …)` が要る）。
- AtomS3 Lite は USB-UART チップが無く、Serial は **ESP32-S3 ネイティブ USB CDC**
  （VID 0x303a）。`-DARDUINO_USB_CDC_ON_BOOT=1`（旧 ATOM 機は 0）。
- USB 書き込みは初回だけ。以後は Ethernet 経由 OTA（`curl` は Win では `curl.exe`）:
  ```bash
  curl -F firmware=@.pio/build/m5atoms3-poe/firmware.bin http://agri-temp-poe-01.local/api/ota
  ```

> 🛠 **ビルド環境（Windows / Linux 共用）・Linux 初回セットアップ（udev / pipx /
> `~/.platformio` を共有しない件）** → フリート共通の一次情報を参照:
> [agri-node-poe-core/docs/cross-platform-build.md](https://github.com/yasunorioi/agri-node-poe-core/blob/main/docs/cross-platform-build.md)

実測: RAM 11.1% / Flash 29.8%（994 KB、pioarduino fork = arduino-esp32 3.x）。

## 初回セットアップ

1. USB で焼く → PoE で給電（PoE ハブ / インジェクタ）
2. DHCP で IP 取得（LED: 青=boot → 赤=リンク無し/リース無し → 緑=OK）
   - **緑にならない/赤のまま**なら W5500 の配線を疑う（上記ピン節）
3. `http://agri-temp-poe-01.local/` を開く
4. `/config` で MQTT Host（`yasu-hp.local`）とスロットを設定
5. Dashboard に温度 → broker で `agriha/2/sensor/WaterTemp` を確認

> **複数台（4台）を立ち上げるとき**: 既定の hostname / node_id は全台
> `agri-temp-poe-01` / `temp_poe_01` で同じ。同一 LAN に同時投入すると mDNS 名衝突＋
> MQTT client-id 衝突（同じ node_id だと broker がどちらかを蹴る）になる。
> **1台ずつ焼いて `/config` で hostname と node_id を一意に**（例 `-01`〜`-04`）
> してから次を繋ぐこと。ハウス割り当て（トピック prefix）は既定 house2 のままでよい。

### LAN に載せられないとき（SoftAP フォールバック）

DHCP リースが取れない状態（ケーブル未接続 / LAN に DHCP が無い / 開封直後で
IP が不明）が起動後 ~15 秒続くと、自動で **provisioning SoftAP** が立つ
（core `AgriProvisionAP`）。Ethernet でリースを取ると自動で落ちる。

1. スマホ/PC の WiFi で SSID **`agri-temp-poe-01`**（= hostname）に接続
   - パスワード: **`agrinode`**（WPA2・全台共通。ビルド時 `-DAGRI_AP_PASSWORD=\"...\"` で変更可）
2. captive portal が自動で `/config` を開く（開かなければ `http://192.168.4.1/`）
3. hostname（= mDNS 名）/ MQTT Host 等を設定して Save
4. Ethernet を繋ぎ直すと SoftAP は自動停止、`http://<新 hostname>.local/` へ

> 有線と同じ `AgriWebUI` をそのまま AP 上に出しているだけなので、設定項目は
> `/config` と完全に同一。W5500(SPI) と WiFi 無線はハード競合せず同時起動できる。

### 状態 LED（G35）

`agri-node-poe-core` と同じ色分け（実装は `led.h`、pin だけ G35）:
青=boot / 赤=リンク無し / **紫=バス空（DS18B20 未検出）** / 橙=MQTT 未接続 / 緑=OK / 白=publish 瞬き

## API（core `AgriWebUI` 提供）

| | |
|---|---|
| `GET /` | Dashboard（3秒ごとに `/api/status` + `/api/dashboard` を更新） |
| `GET /config`・`POST /config` | 設定フォーム（共通＋スロット） |
| `GET /api/status` | 共通（fw/ip/link/mqtt/ccm/uptime/ota）＋ `ow_pin`/`bus_ok`/`probe_count`/`probes[]`/`slots[]` |
| `GET /api/config` | 共通設定 JSON（スロットは `/api/status` 側） |
| `POST /api/ota` | multipart ファーム更新 |
| `GET /ota` / `POST /api/check` / `POST /api/update` | GitHub Release セルフ更新（半自動） |

> DATA ピン変更は**再起動せず**バスを張り替える（`sensorsRebind`）。バス再列挙は 60 秒ごと＋
> 設定保存時にも走るので、旧 wifi 機にあった「Rescan」ボタンは省略。

## セルフ更新（GitHub Release）

core の `AgriOTA` を直接使用（wifi 機の自前 `self_update.h` は不要）。半自動＝起動時＋24h ごとに
Release を確認し、新しければ Dashboard にバナー＋「Update」。押すと `/api/update` で予約し
次の `poll()` で焼いて再起動。

```bash
pio run -e m5atoms3-poe
# asset 名は gh の path#name 記法で固定（Copy-Item 不要・Win/Linux 共通）
gh release create v0.1.0 ".pio/build/m5atoms3-poe/firmware.bin#agri-temp-poe.bin" \
  --title v0.1.0 --notes "..."
```

- タグ = `v` + `FW_VERSION`（`main.cpp`）/ asset 名 = `agri-temp-poe.bin` と完全一致
  （`#agri-temp-poe.bin` がこれを保証）

## 残作業

- 実機での **ビルド確認**（このリポジトリは未ビルド。ファミリー共通の pioarduino fork 前提）。
- 4台それぞれに **一意の hostname / node_id** を付与（初回セットアップの注記参照）。
- 実プローブでの ROM ↔ 実体の対応付け（1本ずつ握って Dashboard で確認）。
- CCM を使うなら yasu-hp の bridge に `sender_override` を1行追加。
