# agri-temp-poe

M5Stack **AtomS3 Lite** ＋ **Atom PoE Base（W5500）** ＋ **DS18B20 × N**（1-Wire マルチドロップ）
の多点温度ノード。`agri-*` ファミリーの **PoE 機**。既定は house2 の水温（`WaterTemp`）。

`agri-temp-wifi` の **PoE 派生**。あちらが WiFi だったのは ATOM U に PoE ベースが
履けないからで、**AtomS3 Lite は Atom PoE ベースに載る**ため、この機は他の
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

### ⚠ W5500（PoE ベース）のピンは AtomS3 用に要確定

`agri-node-poe-core` の W5500 デフォルト（SCK=22 / MISO=23 / MOSI=33 / CS=19）は
**旧 ATOM の底面ピン**で、M5 公式も旧 ATOM でしか記載していない
（docs.m5stack.com/en/atom/atom_poe）。**ESP32-S3 ではこれらの GPIO は自由に使えない**
（26–37 は内蔵フラッシュ/PSRAM）。AtomS3 Lite の底面ヘッダは **G5/G6/G7/G8/G38/G39** で、
PoE ベースの W5500 はこのうち 4 本に落ちる。

`platformio.ini` の `-DW5500_*` build フラグで渡している（既定 `SCK=5 MISO=7 MOSI=8 CS=6`）。
**これは暫定の当て推量**なので、**初回起動前に AtomS3 の回路図 or 実機で確定**すること。
1-Wire ピンと違い SPI には自動探索フォールバックが無い（**DHCP を取れない＝ピンが違う**）。
確定値が分かったら build フラグ 1 か所を直して焼き直す。

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

```powershell
$env:PYTHONIOENCODING="utf-8"
cd C:\Users\kita_\Documents\agri-temp-poe
& "C:\Users\kita_\.platformio\penv\Scripts\pio.exe" run -e m5atoms3-poe
& "C:\Users\kita_\.platformio\penv\Scripts\pio.exe" run -e m5atoms3-poe -t upload
```

- **`agri-node-poe-core` と同じ pioarduino fork**（arduino-esp32 3.x。W5500 の
  `ETH.begin(ETH_PHY_W5500, …)` が要る）。
- AtomS3 Lite は USB-UART チップが無く、Serial は **ESP32-S3 ネイティブ USB CDC**。
  `-DARDUINO_USB_CDC_ON_BOOT=1`（旧 ATOM 機は 0）。
- USB 書き込みは初回だけ。以後は Ethernet 経由 OTA:
  ```powershell
  curl.exe -F firmware=@.pio\build\m5atoms3-poe\firmware.bin http://agri-temp-poe-01.local/api/ota
  ```

## 初回セットアップ

1. USB で焼く → PoE で給電（PoE ハブ / インジェクタ）
2. DHCP で IP 取得（LED: 青=boot → 赤=リンク無し/リース無し → 緑=OK）
   - **緑にならない/赤のまま**なら、まず **W5500 ピン**（上記⚠）を疑う
3. `http://agri-temp-poe-01.local/` を開く
4. `/config` で MQTT Host（`yasu-hp.local`）とスロットを設定
5. Dashboard に温度 → broker で `agriha/2/sensor/WaterTemp` を確認

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

```powershell
& "...\pio.exe" run -e m5atoms3-poe
Copy-Item .pio\build\m5atoms3-poe\firmware.bin agri-temp-poe.bin
gh release create v0.1.0 agri-temp-poe.bin --title "v0.1.0" --notes "..."
```

- タグ = `v` + `FW_VERSION`（`main.cpp`）/ asset 名 = `agri-temp-poe.bin` と完全一致

## 残作業

- **W5500 の AtomS3 ピンを確定**（上記⚠）。DHCP を取れれば正しい。
- 実機での **ビルド確認**（このリポジトリは未ビルド。ファミリー共通の pioarduino fork 前提）。
- 実プローブでの ROM ↔ 実体の対応付け（1本ずつ握って Dashboard で確認）。
- CCM を使うなら yasu-hp の bridge に `sender_override` を1行追加。
