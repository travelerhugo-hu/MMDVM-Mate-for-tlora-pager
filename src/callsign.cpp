#include "callsign.h"
#include "app_bus.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// ---------------------------------------------------------------------------
// Talker Alias sanitising
// ---------------------------------------------------------------------------

static inline bool is_call_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '/' || c == '-';
}

/**
 * Amateur callsign shape test.
 *
 * Deliberately structural rather than a full ITU prefix table: the point is to
 * reject the reassembly garbage listed in the header, not to police licensing.
 * The two rules that actually do the work are "must contain a digit" (kills
 * "My", "Radio", "Mobile") and "must contain a letter" (kills bare numbers).
 */
static bool looks_like_callsign(const char *s)
{
    size_t n = strlen(s);
    if (n < 3 || n > 10) return false;

    bool has_digit = false, has_alpha = false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9')      has_digit = true;
        else if (c >= 'A' && c <= 'Z') has_alpha = true;
        else if (c == '/' || c == '-') { /* portable/mobile suffix */ }
        else return false;             // already upper-cased by the caller
    }
    if (!has_digit || !has_alpha) return false;

    // A run of four or more identical characters is the signature of a
    // half-reassembled TA block ("KQ4IIVIIIVV..."), never a real callsign.
    int run = 1;
    for (size_t i = 1; i < n; ++i) {
        run = (s[i] == s[i - 1]) ? run + 1 : 1;
        if (run >= 4) return false;
    }
    return true;
}

bool callsign_from_talker_alias(const uint8_t *raw, size_t len,
                                char *call, size_t call_sz,
                                char *name, size_t name_sz)
{
    if (!raw || !call || call_sz == 0) return false;
    if (name && name_sz) name[0] = '\0';

    // Pass 1: copy out printable ASCII only, collapsing every run of
    // non-printables (NULs, control bytes) into a single space. That turns
    // "KQ4IIV\0I\0I\0V" into "KQ4IIV I I V" so the tokenizer below sees the
    // first block as its own token instead of a fused mess.
    char clean[CALLSIGN_MAX + TA_NAME_MAX];
    size_t w = 0;
    bool pending_space = false;
    for (size_t i = 0; i < len && w + 1 < sizeof(clean); ++i) {
        char c = (char)raw[i];
        if (c >= 0x20 && c < 0x7F) {
            if (pending_space && w > 0) clean[w++] = ' ';
            pending_space = false;
            if (w + 1 < sizeof(clean)) clean[w++] = c;
        } else {
            pending_space = true;
        }
    }
    clean[w] = '\0';
    if (w == 0) return false;

    // Pass 2: first whitespace-delimited token is the callsign candidate.
    size_t p = 0;
    while (clean[p] == ' ') ++p;

    char cand[16];
    size_t cw = 0;
    while (clean[p] && clean[p] != ' ' && cw + 1 < sizeof(cand)) {
        char c = clean[p++];
        if (!is_call_char(c)) { cw = 0; break; }   // punctuation -> not a call
        cand[cw++] = (char)toupper((unsigned char)c);
    }
    cand[cw] = '\0';
    if (!looks_like_callsign(cand)) return false;

    strlcpy(call, cand, call_sz);

    // Pass 3: whatever follows is the operator name. Collapse runs of spaces
    // and drop trailing whitespace.
    if (name && name_sz > 1) {
        while (clean[p] == ' ') ++p;
        size_t nw = 0;
        bool last_space = false;
        while (clean[p] && nw + 1 < name_sz) {
            char c = clean[p++];
            if (c == ' ') {
                if (last_space || nw == 0) continue;
                last_space = true;
            } else {
                last_space = false;
            }
            name[nw++] = c;
        }
        while (nw > 0 && name[nw - 1] == ' ') --nw;
        name[nw] = '\0';
    }
    return true;
}

// ---------------------------------------------------------------------------
// LRU cache
//
// A flat array with timestamp-based eviction. 256 entries is a linear scan of
// 1 kB of uint32 - a couple of microseconds - which is far cheaper than the
// bug surface of a hash table, and this is only touched once per transmission.
// ---------------------------------------------------------------------------

struct CsEntry {
    uint32_t id;
    uint32_t used;                  // monotonic tick, 0 == empty slot
    char     call[CALLSIGN_MAX];
};

static CsEntry  s_cache[CS_CACHE_MAX];
static uint32_t s_clock = 0;
static portMUX_TYPE s_cache_mux = portMUX_INITIALIZER_UNLOCKED;

bool callsign_cached(uint32_t dmr_id, char *out, size_t out_sz)
{
    if (dmr_id == 0 || !out || out_sz == 0) return false;

    bool hit = false;
    portENTER_CRITICAL(&s_cache_mux);
    for (size_t i = 0; i < CS_CACHE_MAX; ++i) {
        if (s_cache[i].used && s_cache[i].id == dmr_id) {
            s_cache[i].used = ++s_clock;
            strlcpy(out, s_cache[i].call, out_sz);
            hit = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_cache_mux);
    return hit;
}

void callsign_cache_put(uint32_t dmr_id, const char *call)
{
    if (dmr_id == 0 || !call || !call[0]) return;

    portENTER_CRITICAL(&s_cache_mux);
    size_t victim = 0;
    uint32_t oldest = UINT32_MAX;
    for (size_t i = 0; i < CS_CACHE_MAX; ++i) {
        if (s_cache[i].used && s_cache[i].id == dmr_id) { victim = i; goto store; }
        if (s_cache[i].used == 0)                       { victim = i; goto store; }
        if (s_cache[i].used < oldest) { oldest = s_cache[i].used; victim = i; }
    }
store:
    s_cache[victim].id   = dmr_id;
    s_cache[victim].used = ++s_clock;
    strlcpy(s_cache[victim].call, call, CALLSIGN_MAX);
    portEXIT_CRITICAL(&s_cache_mux);
}

// ---------------------------------------------------------------------------
// radioid.net resolver task
// ---------------------------------------------------------------------------

#define LOOKUP_QUEUE_DEPTH 8

static QueueHandle_t s_lookup_q = nullptr;

/// Minimal targeted JSON scan for  "callsign": "XXXXX".
/// Chosen over a JSON library because the response shape is fixed and we would
/// otherwise drag ArduinoJson (and a ~4 kB document buffer) onto a task whose
/// stack is already carrying a TLS session.
static bool extract_callsign(const String &body, char *out, size_t out_sz)
{
    int k = body.indexOf("\"callsign\"");
    if (k < 0) return false;
    k += 10;
    while (k < (int)body.length() && (body[k] == ' ' || body[k] == ':')) ++k;
    if (k >= (int)body.length() || body[k] != '"') return false;
    ++k;
    size_t w = 0;
    while (k < (int)body.length() && body[k] != '"' && w + 1 < out_sz) {
        out[w++] = (char)toupper((unsigned char)body[k++]);
    }
    out[w] = '\0';
    return looks_like_callsign(out);
}

static void lookup_task(void *)
{
    for (;;) {
        uint32_t id = 0;
        if (xQueueReceive(s_lookup_q, &id, portMAX_DELAY) != pdTRUE) continue;
        if (id == 0) continue;

        // Another path (Talker Alias) may have resolved it while we queued.
        char tmp[CALLSIGN_MAX];
        if (callsign_cached(id, tmp, sizeof(tmp))) continue;

        if (WiFi.status() != WL_CONNECTED) continue;

        WiFiClientSecure client;
        // radioid.net is a public read-only directory and we ship no CA bundle;
        // pinning a root here would just break the firmware the next time they
        // rotate certificates. Nothing secret is sent or trusted.
        client.setInsecure();
        client.setTimeout(8);

        HTTPClient http;
        char url[96];
        snprintf(url, sizeof(url),
                 "https://radioid.net/api/dmr/user/?id=%lu", (unsigned long)id);

        if (!http.begin(client, url)) {
            log_w("radioid begin() failed for %lu", (unsigned long)id);
            continue;
        }
        http.setTimeout(8000);
        http.setConnectTimeout(8000);
        int code = http.GET();
        if (code == HTTP_CODE_OK) {
            String body = http.getString();
            char call[CALLSIGN_MAX];
            if (extract_callsign(body, call, sizeof(call))) {
                callsign_cache_put(id, call);
                bus_post_resolved(id, call);
                log_i("radioid %lu -> %s", (unsigned long)id, call);
            } else {
                log_w("radioid %lu: no usable callsign in response", (unsigned long)id);
            }
        } else {
            log_w("radioid %lu: HTTP %d", (unsigned long)id, code);
        }
        http.end();

        // Be a good citizen: radioid.net is a volunteer-run service.
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void callsign_request(uint32_t dmr_id)
{
    if (!s_lookup_q || dmr_id == 0) return;

    char tmp[CALLSIGN_MAX];
    if (callsign_cached(dmr_id, tmp, sizeof(tmp))) return;

    // Non-blocking: if 8 lookups are already pending the network is clearly
    // the bottleneck and the newest ID is not more important than those.
    xQueueSend(s_lookup_q, &dmr_id, 0);
}

bool callsign_begin()
{
    memset(s_cache, 0, sizeof(s_cache));

    s_lookup_q = xQueueCreate(LOOKUP_QUEUE_DEPTH, sizeof(uint32_t));
    if (!s_lookup_q) {
        log_e("lookup queue alloc failed");
        return false;
    }
    BaseType_t rc = xTaskCreatePinnedToCore(lookup_task, "csresolve",
                                            TASK_LOOKUP_STACK, nullptr,
                                            TASK_LOOKUP_PRIO, nullptr,
                                            TASK_LOOKUP_CORE);
    if (rc != pdPASS) {
        log_e("lookup task create failed rc=%d", (int)rc);
        vQueueDelete(s_lookup_q);
        s_lookup_q = nullptr;
        return false;
    }
    return true;
}
