#include "history.h"

#include <Preferences.h>
#include <string.h>

// Flash endurance guard: the persisted slice is rewritten at most this often,
// and only when something actually changed. On a busy talkgroup that turns a
// potential ~2 writes/second into 1 write/minute.
#define HISTORY_SAVE_INTERVAL_MS 60000

#define HIST_NS   "mmdvm"
#define HIST_KEY  "hist"

static QsoEntry s_list[HISTORY_MAX];
static uint8_t  s_count    = 0;
static bool     s_dirty    = false;
static uint32_t s_last_save = 0;

// The on-flash layout is versioned so that a firmware update that changes
// QsoEntry does not resurrect garbage from the previous build.
#define HIST_BLOB_VERSION 1
struct HistBlob {
    uint8_t  version;
    uint8_t  count;
    uint16_t _pad;
    QsoEntry entries[HISTORY_PERSIST];
};

void history_begin()
{
    s_count = 0;
    memset(s_list, 0, sizeof(s_list));

    Preferences p;
    if (!p.begin(HIST_NS, true)) {
        return;                                  // namespace not created yet
    }

    HistBlob blob{};
    size_t got = p.getBytes(HIST_KEY, &blob, sizeof(blob));
    p.end();

    if (got != sizeof(blob) || blob.version != HIST_BLOB_VERSION) {
        return;
    }

    uint8_t n = blob.count;
    if (n > HISTORY_PERSIST) n = HISTORY_PERSIST;

    for (uint8_t i = 0; i < n; ++i) {
        QsoEntry &e = blob.entries[i];
        if (e.dmr_id == 0) continue;             // skip holes
        e.call[CALLSIGN_MAX - 1] = '\0';
        s_list[s_count++] = e;
    }
    log_i("history: restored %u entries", s_count);
}

void history_note(uint32_t dmr_id, uint32_t talkgroup, const char *call)
{
    if (dmr_id == 0) return;

    time_t now = time(nullptr);
    if (now < 1600000000) now = 0;               // clock not disciplined yet

    // Already in the list? Refresh and promote to the front.
    for (uint8_t i = 0; i < s_count; ++i) {
        if (s_list[i].dmr_id != dmr_id) continue;

        QsoEntry hit = s_list[i];
        hit.last_heard = now;
        hit.talkgroup  = talkgroup;
        // Never overwrite a known callsign with an empty one: CALL_START often
        // arrives before the alias has finished reassembling.
        if (call && call[0]) strlcpy(hit.call, call, sizeof(hit.call));

        memmove(&s_list[1], &s_list[0], i * sizeof(QsoEntry));
        s_list[0] = hit;
        s_dirty = true;
        return;
    }

    // New station: push onto the front, dropping the oldest if we are full.
    if (s_count < HISTORY_MAX) s_count++;
    memmove(&s_list[1], &s_list[0], (s_count - 1) * sizeof(QsoEntry));

    QsoEntry &e = s_list[0];
    memset(&e, 0, sizeof(e));
    e.dmr_id     = dmr_id;
    e.talkgroup  = talkgroup;
    e.last_heard = now;
    if (call && call[0]) strlcpy(e.call, call, sizeof(e.call));

    s_dirty = true;
}

bool history_resolve(uint32_t dmr_id, const char *call)
{
    if (!call || !call[0]) return false;

    for (uint8_t i = 0; i < s_count; ++i) {
        if (s_list[i].dmr_id != dmr_id) continue;
        if (strcmp(s_list[i].call, call) == 0) return false;   // nothing new
        strlcpy(s_list[i].call, call, sizeof(s_list[i].call));
        s_dirty = true;
        return true;
    }
    return false;
}

uint8_t history_count()
{
    return s_count;
}

const QsoEntry &history_at(uint8_t index)
{
    static const QsoEntry empty{};
    if (index >= s_count) return empty;
    return s_list[index];
}

bool history_dirty()
{
    return s_dirty;
}

void history_save()
{
    if (!s_dirty) return;

    HistBlob blob{};
    blob.version = HIST_BLOB_VERSION;
    blob.count   = s_count < HISTORY_PERSIST ? s_count : HISTORY_PERSIST;
    memcpy(blob.entries, s_list, blob.count * sizeof(QsoEntry));

    Preferences p;
    if (!p.begin(HIST_NS, false)) {
        log_e("history: NVS open failed");
        return;
    }
    p.putBytes(HIST_KEY, &blob, sizeof(blob));
    p.end();

    s_dirty     = false;
    s_last_save = millis();
}

void history_maintain()
{
    if (!s_dirty) return;
    if (millis() - s_last_save < HISTORY_SAVE_INTERVAL_MS) return;
    history_save();
}

void history_clear()
{
    s_count = 0;
    memset(s_list, 0, sizeof(s_list));
    s_dirty = true;
    history_save();
}
