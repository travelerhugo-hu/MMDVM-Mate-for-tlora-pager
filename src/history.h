/**
 * @file    history.h
 * @brief   "Who have I heard" list - the middle-lower pane of the main screen.
 *
 * Semantics chosen to match how an operator actually reads a monitor screen:
 *
 *  - One row per *station*, not per transmission. A station that keys up ten
 *    times in a row should not push everyone else off the list, so a repeat
 *    caller is moved back to the top and its timestamp refreshed instead of
 *    being appended again.
 *  - Keyed on the DMR ID, because that is the only field CALL_START always
 *    carries. The callsign may still be unknown when the entry is created and
 *    gets patched in later, when either the Talker Alias finishes reassembling
 *    or the radioid.net lookup returns.
 *  - Timestamps are the UTC instant the station *started* transmitting. That
 *    is what a log entry would say, and it is stable - CALL_END can be lost if
 *    the WebSocket drops mid-transmission.
 *
 * The newest HISTORY_PERSIST entries survive a power cycle. Writes are rate
 * limited (see history_maintain) because NVS lives in the same flash as the
 * firmware and this device is expected to run for days.
 */
#pragma once

#include "app_config.h"
#include <time.h>

struct QsoEntry {
    uint32_t dmr_id;
    uint32_t talkgroup;
    time_t   last_heard;                ///< 0 == clock was not synced yet
    char     call[CALLSIGN_MAX];        ///< "" while still unresolved
};

/// Restore the persisted tail of the list. Call after settings_begin().
void history_begin();

/// Record the start of a transmission. `call` may be NULL/"" if not yet known.
void history_note(uint32_t dmr_id, uint32_t talkgroup, const char *call);

/// Late-binding callsign fill-in (Talker Alias or radioid.net). Returns true
/// if an entry actually changed, i.e. if the UI needs a redraw.
bool history_resolve(uint32_t dmr_id, const char *call);

uint8_t history_count();

/// Index 0 is the most recently heard station.
const QsoEntry &history_at(uint8_t index);

/// True if something changed since the last history_save().
bool history_dirty();

/// Force a write-back now (called when leaving the settings screen).
void history_save();

/// Call from loop(). Writes back at most once every HISTORY_SAVE_INTERVAL_MS
/// and only when dirty.
void history_maintain();

void history_clear();
