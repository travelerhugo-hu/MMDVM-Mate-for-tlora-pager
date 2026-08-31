/**
 * @file    app_config.h
 * @brief   MMDVM Mate - global build-time configuration and shared types.
 *
 * Target: LilyGo T-LoRa-Pager (ESP32-S3, 480x222 ST7796, ES8311 codec)
 */
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Firmware identity
// ---------------------------------------------------------------------------
#define APP_NAME            "MMDVM Mate"
#define APP_VERSION         "1.0.0"

// ---------------------------------------------------------------------------
// Display geometry (T-LoRa-Pager panel is a cropped ST7796, landscape)
// ---------------------------------------------------------------------------
#define SCR_W               480
#define SCR_H               222
#define BANNER_TOP_H        30
#define BANNER_BOTTOM_H     26

// ---------------------------------------------------------------------------
// BrandMeister Hoseline "spotter" transport
//
// Protocol reverse-engineered from the official Hoseline web client bundle and
// then confirmed against the live server:
//
//   endpoint  : wss://hose.brandmeister.network/spotter/?token=test:test
//   subproto  : "spotter"          (sent as Sec-WebSocket-Protocol)
//   framing   : MessagePack arrays, element 0 is the type
//
//   Subscribe : [1, [tg, tg, ...]]   <- the talkgroup list is NESTED. A flat
//                                       [1, tg] is silently accepted by the
//                                       server and then delivers nothing; this
//                                       cost a live capture to find.
//   Unsub     : [2, [tg, ...]]       Reset: [3]        Drop TX: [12]
//
//   CALL_START: [11, 0, dmr_id, talkgroup, 0]   -> id at [2], TG at [3]
//   CALL_END  : [13]
//   CALL_AUDIO: [20, <bin 960>]      G.711 u-law, 8 kHz mono == 120 ms
//   CALL_ALIAS: [21, <str>]          DMR Talker Alias, arrives in fragments
//   CALL_METER: [22, <float dB>]
//
// The web client also refuses to subscribe to anything <= 90, so the UI
// enforces the same lower bound.
// ---------------------------------------------------------------------------
#define HOSE_TG_MIN         91
#define HOSE_HOST           "hose.brandmeister.network"
#define HOSE_PORT           443
#define HOSE_PATH           "/spotter/?token=test:test"
#define HOSE_SUBPROTOCOL    "spotter"

// Wire message types (client -> server)
enum : uint8_t {
    HOSE_GROUP_JOIN   = 1,
    HOSE_GROUP_LEAVE  = 2,
    HOSE_GROUP_RESET  = 3,
    HOSE_CALL_DROP    = 12,
};

// Wire message types (server -> client)
enum : uint8_t {
    HOSE_CALL_START    = 11,
    HOSE_CALL_END      = 13,
    HOSE_CALL_AUDIO    = 20,
    HOSE_CALL_ALIAS    = 21,
    HOSE_CALL_METER    = 22,
    HOSE_SYSTEM_RESCUE = 80,
};

// ---------------------------------------------------------------------------
// Audio pipeline
// ---------------------------------------------------------------------------
/// Wire format: G.711 u-law, 8 kHz mono. One byte in == one sample in.
#define AUDIO_IN_RATE       8000

/// The ES8311 is clocked out at 16 kHz and each decoded sample is emitted
/// twice with linear interpolation. Two reasons, both practical:
///   1. 8 kHz needs MCLK=2.048 MHz, the very bottom of the ES8311 coefficient
///      table; 16 kHz is the rate the vendor BSP is actually validated at.
///   2. Interpolating in software costs ~2 % CPU and audibly softens the
///      quantisation buzz that 8 kHz u-law has on a tiny speaker.
#define AUDIO_OUT_RATE      16000
#define AUDIO_UPSAMPLE      (AUDIO_OUT_RATE / AUDIO_IN_RATE)
#define AUDIO_BITS          16
#define AUDIO_CHANNELS      1

/// Jitter buffer holds raw u-law bytes (1 byte == 1 sample == 125 us).
/// 12000 bytes == 1.5 s of speech. Stored as a FreeRTOS stream buffer, which
/// is a lock-free single-producer/single-consumer primitive.
#define AUDIO_RING_BYTES    12000

/// Playback only starts once this much audio is queued, so that short network
/// stalls do not cause dropouts. 2400 bytes == 300 ms.
#define AUDIO_PREBUFFER     2400

/// Audio task drains the ring in 40 ms blocks (320 u-law bytes -> 640 PCM).
#define AUDIO_BLOCK_SAMPLES 320

/// If the ring stays empty this long while a call is active, fall back to
/// re-buffering instead of stuttering.
#define AUDIO_UNDERRUN_MS   500

// ---------------------------------------------------------------------------
// Task configuration
//
// Stack sizes are deliberate, not guessed: they were sized from the deepest
// call chain of each task (TLS handshake for the network task dominates) and
// are verified at runtime through uxTaskGetStackHighWaterMark() - see
// diag_report_stacks() in main.cpp.
// ---------------------------------------------------------------------------
#define TASK_NET_STACK      12288   // mbedTLS handshake + websocket + msgpack
#define TASK_NET_PRIO       4
#define TASK_NET_CORE       0

#define TASK_AUDIO_STACK    4096
#define TASK_AUDIO_PRIO     6       // highest: hard real-time I2S feed
#define TASK_AUDIO_CORE     1

#define TASK_LOOKUP_STACK   10240   // one TLS session for radioid.net
#define TASK_LOOKUP_PRIO    2
#define TASK_LOOKUP_CORE    0

// ---------------------------------------------------------------------------
// Application limits
// ---------------------------------------------------------------------------
#define CALLSIGN_MAX        16      // DMR talker alias callsign field
#define TA_NAME_MAX         24
#define HISTORY_MAX         32      // QSO history entries kept in RAM
#define HISTORY_PERSIST     16      // entries written back to NVS
#define CS_CACHE_MAX        256     // DMR-ID -> callsign LRU cache entries
#define WIFI_SSID_MAX       33
#define WIFI_PASS_MAX       65

// ---------------------------------------------------------------------------
// Backlight / screen blanking
// ---------------------------------------------------------------------------
/// The AW9364 charge pump on this board exposes 16 discrete steps.
#define BL_LEVEL_MAX        16
static const uint8_t BL_STEPS[4]      = { 4, 8, 12, 16 };   // 25/50/75/100 %
static const uint8_t BL_PERCENTS[4]   = { 25, 50, 75, 100 };

/// Screen blanking choices, in seconds. 0 == never blank.
static const uint16_t SLEEP_SECONDS[5] = { 30, 60, 180, 300, 0 };
static const char *const SLEEP_LABELS[5] = { "30s", "60s", "3min", "5min", "Always On" };

// ---------------------------------------------------------------------------
// Shared runtime state pushed from the network task to the UI task.
// ---------------------------------------------------------------------------
enum UiEventType : uint8_t {
    EV_CALL_START,
    EV_CALL_END,
    EV_TALKER_ALIAS,
    EV_VU_METER,
    EV_LINK_STATE,
    EV_CALLSIGN_RESOLVED,
    EV_ROTARY,
};

/// Payload of EV_ROTARY (carried in UiEvent::code).
///
/// The encoder is *not* read from the UI task. LilyGoLoRaPager::getRotary()
/// blocks for up to 50 ms on an empty queue, so polling it from loop() would
/// peg the whole UI at ~20 fps. Instead a tiny dedicated task blocks on it and
/// republishes here, which costs 2 kB of stack and keeps the UI at full rate.
enum RotaryCode : uint8_t {
    ROT_CW,
    ROT_CCW,
    ROT_CLICK,
};

enum LinkState : uint8_t {
    LINK_WIFI_DOWN,
    LINK_WIFI_UP,
    LINK_HOSE_CONNECTING,
    LINK_HOSE_UP,
    LINK_HOSE_DOWN,
};

struct UiEvent {
    UiEventType type;
    uint8_t     link;                       // EV_LINK_STATE  (LinkState)
    uint8_t     code;                       // EV_ROTARY      (RotaryCode)
    uint32_t    dmr_id;                     // EV_CALL_START / EV_CALLSIGN_RESOLVED
    uint32_t    talkgroup;                  // EV_CALL_START
    float       vu_db;                      // EV_VU_METER
    char        call[CALLSIGN_MAX];         // resolved callsign, "" if unknown
    char        name[TA_NAME_MAX];          // operator name from Talker Alias
};

// ---------------------------------------------------------------------------
// Rotary reader task (see RotaryCode above)
// ---------------------------------------------------------------------------
#define TASK_ROT_STACK      2048
#define TASK_ROT_PRIO       3
#define TASK_ROT_CORE       1
