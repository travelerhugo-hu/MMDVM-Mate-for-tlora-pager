#include "hose_client.h"
#include "app_bus.h"
#include "audio_player.h"
#include "callsign.h"
#include "msgpack_lite.h"
#include "settings.h"
#include "wifi_time.h"

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static WebSocketsClient   s_ws;
static volatile uint32_t  s_want_tg       = 0;
static volatile bool      s_tg_dirty      = true;
static volatile bool      s_reconnect_req = false;
static volatile LinkState s_link          = LINK_WIFI_DOWN;

static uint32_t s_subscribed_tg = 0;
static uint32_t s_call_dmr_id   = 0;
static bool     s_call_active   = false;
static uint32_t s_last_rx_ms    = 0;

// Set once per call: stop re-parsing Talker Alias fragments after the first
// fragment that yields a valid callsign. Later fragments only repeat or
// corrupt it.
static bool s_alias_locked = false;

// ---------------------------------------------------------------------------

static void set_link(LinkState st)
{
    if (s_link == st) return;
    s_link = st;
    bus_post_link(st);
}

LinkState hose_link_state() { return s_link; }

void hose_set_talkgroup(uint32_t tg)
{
    if (tg < HOSE_TG_MIN) return;
    s_want_tg  = tg;
    s_tg_dirty = true;
}

void hose_request_reconnect() { s_reconnect_req = true; }

// ---------------------------------------------------------------------------
// Outbound frames
// ---------------------------------------------------------------------------

/// [3] - drop every existing subscription.
static void send_reset()
{
    uint8_t buf[8];
    MsgPackWriter w(buf, sizeof(buf));
    w.writeArrayHeader(1);
    w.writeUInt(HOSE_GROUP_RESET);
    if (w.overflowed()) return;
    s_ws.sendBIN(w.data(), w.size());
}

/// [1, [tg]] - note the nested array; a flat [1, tg] connects fine and then
/// silently delivers nothing.
static void send_subscribe(uint32_t tg)
{
    uint8_t buf[16];
    MsgPackWriter w(buf, sizeof(buf));
    w.writeArrayHeader(2);
    w.writeUInt(HOSE_GROUP_JOIN);
    w.writeArrayHeader(1);
    w.writeUInt(tg);
    if (w.overflowed()) {
        log_e("subscribe frame overflow");
        return;
    }
    if (s_ws.sendBIN(w.data(), w.size())) {
        log_i("subscribed to TG %lu", (unsigned long)tg);
    } else {
        log_w("subscribe send failed");
    }
}

// ---------------------------------------------------------------------------
// Inbound frames
// ---------------------------------------------------------------------------

static void on_call_start(MsgPackReader &r, uint32_t elems)
{
    // [11, 0, dmr_id, talkgroup, 0]
    if (elems < 4) return;

    uint64_t pad = 0, id = 0, tg = 0;
    if (!r.readUInt(pad)) return;
    if (!r.readUInt(id))  return;
    if (!r.readUInt(tg))  return;

    s_call_dmr_id  = (uint32_t)id;
    s_call_active  = true;
    s_alias_locked = false;

    // A new transmission must never inherit the tail of the previous one.
    audio_flush();

    char call[CALLSIGN_MAX] = {0};
    if (!callsign_cached(s_call_dmr_id, call, sizeof(call))) {
        call[0] = '\0';
        callsign_request(s_call_dmr_id);   // answers later, asynchronously
    }
    bus_post_call_start(s_call_dmr_id, (uint32_t)tg, call);
}

static void on_call_alias(MsgPackReader &r, uint32_t elems)
{
    if (elems < 2 || s_alias_locked) return;

    const uint8_t *p = nullptr;
    uint32_t len = 0;
    if (!r.readBytes(&p, len) || len == 0) return;

    char call[CALLSIGN_MAX];
    char name[TA_NAME_MAX];
    if (!callsign_from_talker_alias(p, len, call, sizeof(call), name, sizeof(name))) {
        return;     // half-assembled fragment - wait for the next one
    }

    s_alias_locked = true;
    if (s_call_dmr_id) callsign_cache_put(s_call_dmr_id, call);
    bus_post_alias(call, name);
}

static void handle_frame(const uint8_t *data, size_t len)
{
    s_last_rx_ms = millis();

    MsgPackReader r(data, len);
    uint32_t elems = 0;
    if (!r.readArrayHeader(elems) || elems == 0) return;

    uint64_t type = 0;
    if (!r.readUInt(type)) return;

    switch (type) {
    case HOSE_CALL_START:
        on_call_start(r, elems);
        break;

    case HOSE_CALL_END:
        s_call_active  = false;
        s_call_dmr_id  = 0;
        s_alias_locked = false;
        bus_post_call_end();
        break;

    case HOSE_CALL_AUDIO: {
        if (elems < 2) break;
        const uint8_t *p = nullptr;
        uint32_t n = 0;
        if (!r.readBytes(&p, n) || n == 0) break;
        audio_push(p, n);          // zero-copy straight into the jitter buffer
        break;
    }

    case HOSE_CALL_ALIAS:
        on_call_alias(r, elems);
        break;

    case HOSE_CALL_METER: {
        if (elems < 2) break;
        double db = 0;
        if (r.readDouble(db)) bus_post_meter((float)db);
        break;
    }

    case HOSE_SYSTEM_RESCUE:
        // Server-side restart notice: our subscription is gone with it.
        log_w("server sent SYSTEM_RESCUE, re-subscribing");
        s_tg_dirty = true;
        break;

    default:
        break;
    }
}

static void ws_event(WStype_t type, uint8_t *payload, size_t length)
{
    switch (type) {
    case WStype_CONNECTED:
        log_i("Hoseline connected");
        set_link(LINK_HOSE_UP);
        s_last_rx_ms    = millis();
        s_subscribed_tg = 0;
        s_tg_dirty      = true;     // (re)subscribe from the task context
        break;

    case WStype_DISCONNECTED:
        log_w("Hoseline disconnected");
        set_link(wifi_is_connected() ? LINK_HOSE_DOWN : LINK_WIFI_DOWN);
        if (s_call_active) {
            s_call_active = false;
            audio_flush();
            bus_post_call_end();
        }
        break;

    case WStype_BIN:
        handle_frame(payload, length);
        break;

    case WStype_TEXT:
        // The spotter endpoint is binary-only; a text frame means the server
        // is telling us something went wrong.
        log_w("unexpected text frame: %.*s", (int)length, (const char *)payload);
        break;

    case WStype_ERROR:
        log_e("websocket error");
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Network task
// ---------------------------------------------------------------------------

static void net_task(void *)
{
    bool ws_started = false;
    uint32_t wifi_retry_at = 0;

    s_want_tg = g_settings.talkgroup;

    for (;;) {
        uint32_t now = millis();

        // --- 1. Wi-Fi ------------------------------------------------------
        if (!wifi_is_connected()) {
            if (ws_started) {
                s_ws.disconnect();
                ws_started = false;
            }
            set_link(LINK_WIFI_DOWN);
            if ((int32_t)(now - wifi_retry_at) >= 0) {
                wifi_retry_at = now + 15000;
                wifi_begin_connect();
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        // --- 2. WebSocket session ------------------------------------------
        if (s_reconnect_req) {
            s_reconnect_req = false;
            if (ws_started) {
                s_ws.disconnect();
                ws_started = false;
            }
        }

        if (!ws_started) {
            set_link(LINK_HOSE_CONNECTING);
            s_ws.beginSSL(HOSE_HOST, HOSE_PORT, HOSE_PATH, "", HOSE_SUBPROTOCOL);
            s_ws.onEvent(ws_event);
            s_ws.setReconnectInterval(5000);
            // Ping every 20 s, expect a pong within 5 s, give up after two
            // misses. The server answers RFC 6455 pings, and this is the only
            // way to notice a silently dropped NAT mapping while the talkgroup
            // is quiet (an idle TG sends literally nothing).
            s_ws.enableHeartbeat(20000, 5000, 2);
            ws_started   = true;
            s_last_rx_ms = now;
        }

        s_ws.loop();

        // --- 3. Subscription upkeep ----------------------------------------
        if (s_ws.isConnected()) {
            uint32_t want = s_want_tg;
            if (s_tg_dirty || want != s_subscribed_tg) {
                s_tg_dirty = false;
                if (want >= HOSE_TG_MIN) {
                    send_reset();
                    send_subscribe(want);
                    s_subscribed_tg = want;
                    audio_flush();
                    if (s_call_active) {
                        s_call_active = false;
                        bus_post_call_end();
                    }
                }
            }
        }

        // 1 tick of slack keeps this task from starving the idle task while
        // still allowing ~1000 frames/s - eight times the 8.3 fps audio rate.
        vTaskDelay(1);
    }
}

bool hose_begin()
{
    BaseType_t rc = xTaskCreatePinnedToCore(net_task, "hosenet",
                                            TASK_NET_STACK, nullptr,
                                            TASK_NET_PRIO, nullptr,
                                            TASK_NET_CORE);
    if (rc != pdPASS) {
        log_e("net task create failed rc=%d", (int)rc);
        return false;
    }
    return true;
}
