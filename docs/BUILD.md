# Build & flash guide (for builders)

This document holds the developer-facing details that are intentionally kept out of
the top-level `README.md` so the project landing page stays friendly to casual
readers. If you just want to *use* the device, the main README is enough.

---

## What it does

| Feature               | Notes                                                                                         |
| --------------------- | --------------------------------------------------------------------------------------------- |
| Live talkgroup audio  | Streams from `hose.brandmeister.network` over WSS, decoded to the ES8311 codec                |
| Callsign display      | Shows the active operator's callsign (or `FREE` when the channel is clear)                    |
| UTC + battery banner  | Kept accurate via NTP/SNTP, seeded from the on-board RTC between syncs                        |
| QSO history           | Last 32 contacts, persisted to NVS across reboots                                             |
| Callsign resolution   | DMR Talker Alias (fast) **and** `radioid.net` lookup (authoritative), LRU-cached              |
| Settings UI           | Wi-Fi, talkgroup, volume, backlight, screen-off timeout, reboot — all NVS-backed              |
| Hardware power saving | LoRa / GNSS / NFC / SD / mic disabled; RTC, codec, keyboard, encoder, fuel gauge, PMU kept on |

---

## Hardware

Target board: **LilyGo T-LoRa Pager** (`lilygo-t-lora-pager`).

| Resource      | Detail                                                         |
| ------------- | -------------------------------------------------------------- |
| MCU           | ESP32-S3 (240 MHz dual-core Xtensa LX7)                        |
| Flash / PSRAM | 16 MB / 8 MB                                                   |
| Display       | 480 × 222 px ST7796 (cropped landscape, 16-bit RGB)            |
| Audio         | ES8311 codec (I2S, mono)                                       |
| Input         | TCA8418 4×10 matrix keyboard + rotary encoder w/ centre button |
| Power         | PMU (AXP / SY6973) + fuel gauge, ~1500 mAh cell                |

Peripherals **intentionally disabled** at boot (`HW_DISABLE_MASK` in `main.cpp`):
`NO_HW_LORA | NO_HW_GPS | NO_HW_NFC | NO_HW_SD | NO_HW_MIC | NO_INIT_FATFS | NO_SCAN_I2C_DEV`.
This saves ~1.4 s of boot time and keeps the SX1262 and GNSS powered down.

---

## Architecture

MMDVM Mate is a small **FreeRTOS** application built on the Arduino-ESP32 core
(via PlatformIO). LVGL is compiled with `LV_USE_OS == LV_OS_NONE` (no internal locking),
so **only the Arduino `loop()` task ever touches LVGL**. Worker tasks communicate with the
screen exclusively through a lock-free event queue (`app_bus`).

### Task map

| Task                          | Core / Prio | Responsibility                                                         |
| ----------------------------- | ----------- | ---------------------------------------------------------------------- |
| `loopTask` (Arduino `loop()`) | 1 / 1       | LVGL, keyboard polling, event dispatch, screen blanking                |
| `rotary`                      | 1 / 3       | Blocks on `LilyGoLoRaPager::getRotary()`, republishes detents/clicks   |
| `audio`                       | 1 / 6       | **Highest** — u-law decode + 2× upsample → ES8311 I2S feed             |
| `hosenet`                     | 0 / 4       | TLS + WebSocket + MessagePack decode → audio jitter buffer + UI events |
| `csresolve`                   | 0 / 2       | `radioid.net` HTTPS lookups for unknown DMR IDs                        |

### Audio pipeline

```
[network task] --audio_push()--> StreamBuffer (u-law bytes) --> [audio task]
                                                                   |
                                                 decode + 2× linear upsample  |
                                                                   v
                                                          instance.codec.write()
```

The jitter buffer is a FreeRTOS **StreamBuffer** (single-producer / single-consumer, lock-free).
The producer never blocks: if the ring is full it drops the frame and counts it, because
stalling the WebSocket task would back up TCP and degrade the *next* second of audio.

- Wire format: **G.711 µ-law, 8 kHz mono** (1 byte = 1 sample = 125 µs).
- Output: **16 kHz** — the ES8311 is clocked at 16 kHz (the rate the vendor BSP is validated
  at) and each decoded sample is emitted twice with linear interpolation (softens the
  µ-law quantisation buzz on a small speaker).
- Ring holds 12 000 bytes (1.5 s); playback starts only after 2 400 bytes (300 ms) are
  queued, so short network stalls don't cause dropouts.

---

## The BrandMeister Hoseline "spotter" protocol

Reverse-engineered from the official Hoseline web client and confirmed against the live server.

```
endpoint : wss://hose.brandmeister.network/spotter/?token=test:test
subproto : "spotter"                       (sent as Sec-WebSocket-Protocol)
framing  : MessagePack arrays, element [0] is the message type
```

> **⚠️ The demo token `test:test` is anonymous and server-limited.** It is fine for
> evaluation; for reliable monitoring use your own BrandMeister API token.

### Wire message types

| Dir | Type               | Payload                  | Meaning                               |
| --- | ------------------ | ------------------------ | ------------------------------------- |
| C→S | `1` GROUP_JOIN     | `[1, [tg, ...]]`         | **Nested** talkgroup list             |
| C→S | `2` GROUP_LEAVE    | `[2, [tg, ...]]`         | Unsubscribe                           |
| C→S | `3` GROUP_RESET    | `[3]`                    | Reset subscription                    |
| C→S | `12` CALL_DROP     | `[12]`                   | Drop current TX                       |
| S→C | `11` CALL_START    | `[11, 0, dmr_id, tg, 0]` | id at `[2]`, TG at `[3]`              |
| S→C | `13` CALL_END      | `[13]`                   |                                       |
| S→C | `20` CALL_AUDIO    | `[20, <bin 960>]`        | 960 µ-law bytes = 120 ms @ 8 kHz      |
| S→C | `21` CALL_ALIAS    | `[21, <str>]`            | DMR Talker Alias (arrives fragmented) |
| S→C | `22` CALL_METER    | `[22, <float dB>]`       | Signal strength                       |
| S→C | `80` SYSTEM_RESCUE | `[80, ...]`              | Server-side keepalive / rescue        |

**Two gotchas that cost live captures to find:**

1. The talkgroup list in `GROUP_JOIN` is **nested**: `[1, [tg]]`, not `[1, tg]`.
   A flat payload is *silently* accepted by the server but delivers nothing.
2. The web client refuses talkgroups **≤ 90**, so the UI enforces the same lower bound
   (`HOSE_TG_MIN = 91`).

---

## Project layout

```
MMDVM-Mate/
├── platformio.ini          # build config (see "Building" — platform pin is critical)
├── partitions.csv          # OTA partition table (nvs/otadata/app0/app1/ffat/coredump)
├── boards/
│   └── lilygo-t-lora-pager.json   # board definition (used by PlatformIO)
├── include/
│   └── lv_conf.h           # LVGL 9.2.2 config (LV_OS_NONE, 16-bit, 96 kB draw mem)
└── src/
    ├── main.cpp            # entry point + the single UI task (loop)
    ├── app_config.h        # global config, shared types, task/stack sizes, protocol constants
    ├── app_bus.h/.cpp      # lock-free event queue between workers and the UI
    ├── hose_client.h/.cpp  # WSS "spotter" client: Wi-Fi, TLS, MessagePack, jitter feed
    ├── msgpack_lite.h/.cpp # minimal MessagePack array decoder (no allocator)
    ├── audio_player.h/.cpp # u-law → ES8311 playback chain + StreamBuffer
    ├── callsign.h/.cpp     # Talker Alias parsing + radioid.net lookup + LRU cache
    ├── wifi_time.h/.cpp    # Wi-Fi association, SNTP/NTP, RTC seeding
    ├── settings.h/.cpp     # NVS-backed user settings (single POD struct)
    ├── history.h/.cpp      # QSO history (RAM ring + NVS persistence)
    └── ui.h/.cpp           # LVGL front end: main monitor + settings screens
```

---

## Building

### Prerequisites

- [PlatformIO Core](https://platformio.org/install/cli) (CLI) **or** PlatformIO IDE.
- Python 3.
- A serial driver for the ESP32-S3 (CP210x / CH343).

### ⚠️ Platform / toolchain requirement (read this first)

LilyGoLib **hard-requires Arduino-ESP32 core ≥ 3.3.0 (ESP-IDF 5.x)**. The stock
`platformio/espressif32` 6.x packages ship **Arduino core 2.0.17 (IDF 4.x)**, whose
`esp_lcd` driver is missing the IDF-5 fields LilyGoLib's display init uses
(`rgb_ele_order`, `data_endian`, `color_space`), so the build fails with:

```
LilyGoDispInterface.cpp:449: 'esp_lcd_panel_dev_config_t' has no member named 'rgb_ele_order'
```

The vendor's own PlatformIO template still pins `espressif32@6.10.0` and is therefore stale.

**This project pins the community `pioarduino` platform, which bundles current cores:**

```
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip
```

`55.03.311` == **Arduino 3.3.11 / ESP-IDF 5.5.5**. Do **not** "upgrade" the platform to a
`platformio/espressif32` 6.x release or the build will break.

### Build commands

```bash
# from the MMDVM-Mate/ directory
pio run -e tlora_pager
```

Key `build_flags` (already in `platformio.ini`, documented for maintainers):

| Flag                                                  | Why                                                                                                                                                                                                       |
| ----------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `-D LV_CONF_PATH=${platformio.include_dir}/lv_conf.h` | PlatformIO does not put the project `include/` on the *library* compile path, so LVGL's `lv_conf.h` probe misses ours; an absolute path fixes it (lvgl stringifies it itself — do **not** quote it here). |
| `-D BOARD_HAS_PSRAM`                                  | LVGL draw buffers + audio jitter buffer live in PSRAM.                                                                                                                                                    |
| `-D ARDUINO_LILYGO_LORA_SX1262`                       | Tells LilyGoLib which radio variant is fitted.                                                                                                                                                            |
| `-D RADIOLIB_EXCLUDE_*`                               | Strip unused RadioLib modems — saves significant flash.                                                                                                                                                   |
| `lib_ldf_mode = deep`                                 | **Not** `deep+`: the `+` mode evaluates preprocessor conditionals and then fails to find `Adafruit_TCA8418.h` (guarded by `USING_INPUT_DEV_KEYBOARD` in the board variant header the LDF never parses).   |

### Output

On success:

```
.pio/build/tlora_pager/firmware.bin            # application image
.pio/build/tlora_pager/firmware.factory.bin    # bootloader + partitions + app (flash at 0x0)
.pio/build/tlora_pager/bootloader.bin
.pio/build/tlora_pager/partitions.bin
```

Typical footprint: ~44 % flash, ~20 % RAM.

---

## Flashing

### Option A — combined factory image (recommended)

`firmware.factory.bin` already contains the bootloader, partition table and application.
One command, no offset bookkeeping:

```bash
esptool.py --chip esp32s3 -p <PORT> -b 921600 \
  write_flash 0x0 .pio/build/tlora_pager/firmware.factory.bin
```

### Option B — flash components individually

```bash
esptool.py --chip esp32s3 -p <PORT> -b 921600 \
  write_flash 0x0000  .pio/build/tlora_pager/bootloader.bin \
             0x8000  .pio/build/tlora_pager/partitions.bin \
             0x10000 .pio/build/tlora_pager/firmware.bin
```

> If your toolchain also emits `boot_app0.bin`, flash it at `0xe000`. The factory image
> (Option A) already includes everything.

### Monitor

```bash
pio device monitor -b 115200
```

Use `monitor_filters = default, esp32_exception_decoder` (already set) to get decoded
backtraces on crash.

---

## Controls

**Keyboard (TCA8418 matrix):**

| Key | Action                                         |
| --- | ---------------------------------------------- |
| `S` | Open / close **Settings**                      |
| `L` | Cycle **backlight** 25 % → 50 % → 75 % → 100 % (the bottom bar shows the level for 5 s, then returns to the shortcut legend) |
| `Q` | **Volume −** 5 %                               |
| `E` | **Volume +** 5 %                               |
| **Orange key** (left of space) | Toggle **Number/Symbol mode** (`abc` ↔ `123`) — in `123` mode, letter keys type the digit/symbol printed on them. The orange key is **not** a backlight control. |
| **SHIFT/CAPS key** (bottom-right) | Toggle **Caps Lock** — letters come out uppercase |

**Rotary encoder:**

| Action              | Result                         |
| ------------------- | ------------------------------ |
| Rotate CW / CCW     | Scroll the QSO history         |
| Press centre        | Open **Settings**              |
| In Settings: rotate | Same as `Q` / `E` (step value) |
| In Settings: click  | Same as `Enter`                |

> Incoming traffic **does not** wake or hold the screen — only user input counts as
> activity, so the backlight timeout actually saves battery on a busy talkgroup.

> **Keyboard firmware patches (LilyGoLib).** The stock keyboard driver is patched
> in `libdeps/<env>/LilyGoLib/src/`:
> - The **orange key** (`symbol_key_value = 0x1E`) emits a sentinel
>   (`KB_SYM_SENTINEL`) instead of toggling the library's internal symbol mode.
>   `ui.cpp` owns the `abc` ↔ `123` toggle, so the `S`/`L`/`Q`/`E` control keys
>   are never remapped.
> - The library's **keyboard backlight control** is disabled (`alt_key_value = 0xFF`),
>   because backlight brightness is owned by the `L` key in the firmware.

### Settings

Open with `S` (or encoder click). Navigate with `Q`/`E`, activate with `Enter`,
exit with `S`/`Backspace`.

| Row                | What it does                                                                                                                |
| ------------------ | --------------------------------------------------------------------------------------------------------------------------- |
| **Wi-Fi SSID**     | Text field — type SSID, `Enter` to save, triggers reconnect                                                                 |
| **Wi-Fi Password** | Text field — type password, `Enter` to save, triggers reconnect                                                             |
| **Talkgroup**      | **Manual digit entry** — press the orange key once to enter `123` mode, then type the TG number (digits only, min `91`), `Enter` to save. Changing it re-subscribes immediately |
| **Volume**         | 0–100 % stepper                                                                                                             |
| **Backlight**      | 25 / 50 / 75 / 100 %                                                                                                        |
| **Screen off**     | 30 s / 60 s / 3 min / 5 min / Always On                                                                                     |
| **Reboot**         | Restart the device                                                                                                          |

All settings persist to **NVS** and survive power loss.

---

## Configuration knobs

Most tunables live in `src/app_config.h`:

| Define                            | Default                                                   | Purpose                                                           |
| --------------------------------- | --------------------------------------------------------- | ----------------------------------------------------------------- |
| `HOSE_TG_MIN`                     | `91`                                                      | Minimum talkgroup the UI accepts                                  |
| `HOSE_HOST` / `HOSE_PATH`         | `hose.brandmeister.network` / `/spotter/?token=test:test` | Endpoint + token                                                  |
| `AUDIO_RING_BYTES`                | `12000`                                                   | Jitter buffer size (1.5 s)                                        |
| `AUDIO_PREBUFFER`                 | `2400`                                                    | Bytes queued before playback starts (300 ms)                      |
| `AUDIO_UNDERRUN_MS`               | `500`                                                     | Re-buffer if the ring stays empty this long                       |
| `BL_STEPS[]`                      | `{4,8,12,16}`                                             | Discrete backlight hardware levels                                |
| `SLEEP_SECONDS[]`                 | `{30,60,180,300,0}`                                       | Screen-off timeout choices                                        |
| `HISTORY_MAX` / `HISTORY_PERSIST` | `32` / `16`                                               | RAM ring / NVS-persisted entries                                  |
| `CS_CACHE_MAX`                    | `256`                                                     | DMR-ID → callsign LRU cache size                                  |
| `TASK_*_STACK`                    | see file                                                  | Per-task stack sizes (verified via `uxTaskGetStackHighWaterMark`) |

---

## Diagnostics

The firmware logs periodically over the serial console (115200 baud):

```
heap <free> / psram <free> / loop stack <words left> / audio drops <n>
```

- **Stack high-water marks** are logged from `diag_report()` — if you change a task's
  workload, watch for the watermark dropping toward zero.
- **Audio drops** (`audio_dropped_bytes()`) indicate the jitter buffer overflowed because
  the network task couldn't keep up — usually a Wi-Fi/link issue, not a code bug.

Enable `monitor_filters = ... esp32_exception_decoder` to get human-readable crash
backtraces in the monitor.

---

## Limitations & notes

- **Token:** the built-in `test:test` token is anonymous and rate/feature limited.
  Replace `HOSE_PATH` with your own BrandMeister API token for production use.
- **Audio quality:** 8 kHz µ-law is the format the spotter feed provides; it is upscaled
  to 16 kHz for the codec. Do not expect FM-HiFi.
- **Not a transceiver:** MMDVM Mate only *monitors*. It cannot transmit.
- **No affiliation:** this project is an independent hobby build and is not endorsed by
  BrandMeister, LilyGo, or any amateur-radio organisation.
- **Legal:** only monitor talkgroups you are licensed / authorised to receive in your
  jurisdiction.

---

## License

This project is released under the **MIT License** — see [`LICENSE`](../LICENSE).

It depends on third-party components with their own licenses:
[LilyGoLib](https://github.com/Xinyuan-LilyGO/LilyGoLib),
[RadioLib](https://github.com/jgromes/RadioLib),
[LVGL](https://github.com/lvgl/lvgl),
[WebSockets](https://github.com/Links2004/arduinoWebSockets),
[TinyGPSPlus](https://github.com/mikalhart/TinyGPSPlus),
[Adafruit TCA8418](https://github.com/adafruit/Adafruit_TCA8418),
and the ST25R3916 / NFC-RFAL forks — please respect their respective terms.
