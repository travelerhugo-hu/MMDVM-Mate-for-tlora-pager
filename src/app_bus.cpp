#include "app_bus.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// 24 slots x sizeof(UiEvent) (~56 B) == ~1.4 kB. Deep enough to absorb the
// CALL_START + several CALL_ALIAS fragments that arrive back to back at the
// head of every transmission.
#define UI_QUEUE_DEPTH 24

static QueueHandle_t s_q = nullptr;

bool bus_begin()
{
    s_q = xQueueCreate(UI_QUEUE_DEPTH, sizeof(UiEvent));
    if (!s_q) {
        log_e("UI queue alloc failed");
        return false;
    }
    return true;
}

bool bus_post(const UiEvent &ev)
{
    if (!s_q) return false;
    return xQueueSend(s_q, &ev, 0) == pdTRUE;
}

bool bus_recv(UiEvent &ev, TickType_t ticks)
{
    if (!s_q) return false;
    return xQueueReceive(s_q, &ev, ticks) == pdTRUE;
}

// --- typed helpers ---------------------------------------------------------

static inline void ev_clear(UiEvent &e, UiEventType t)
{
    memset(&e, 0, sizeof(e));
    e.type = t;
}

bool bus_post_link(LinkState st)
{
    UiEvent e; ev_clear(e, EV_LINK_STATE);
    e.link = (uint8_t)st;
    return bus_post(e);
}

bool bus_post_call_start(uint32_t dmr_id, uint32_t talkgroup, const char *call)
{
    UiEvent e; ev_clear(e, EV_CALL_START);
    e.dmr_id    = dmr_id;
    e.talkgroup = talkgroup;
    if (call) strlcpy(e.call, call, sizeof(e.call));
    return bus_post(e);
}

bool bus_post_call_end()
{
    UiEvent e; ev_clear(e, EV_CALL_END);
    return bus_post(e);
}

bool bus_post_alias(const char *call, const char *name)
{
    UiEvent e; ev_clear(e, EV_TALKER_ALIAS);
    if (call) strlcpy(e.call, call, sizeof(e.call));
    if (name) strlcpy(e.name, name, sizeof(e.name));
    return bus_post(e);
}

bool bus_post_resolved(uint32_t dmr_id, const char *callsign)
{
    UiEvent e; ev_clear(e, EV_CALLSIGN_RESOLVED);
    e.dmr_id = dmr_id;
    if (callsign) strlcpy(e.call, callsign, sizeof(e.call));
    return bus_post(e);
}

bool bus_post_meter(float db)
{
    UiEvent e; ev_clear(e, EV_VU_METER);
    e.vu_db = db;
    return bus_post(e);
}

bool bus_post_rotary(RotaryCode code)
{
    UiEvent e; ev_clear(e, EV_ROTARY);
    e.code = (uint8_t)code;
    return bus_post(e);
}
