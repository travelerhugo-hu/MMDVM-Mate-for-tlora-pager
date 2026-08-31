/**
 * @file    app_bus.h
 * @brief   One-way event queue: worker tasks -> UI task.
 *
 * Everything the UI shows is derived from UiEvent messages posted here. That
 * keeps every LVGL call on a single task (LVGL is not thread-safe and this
 * build has LV_USE_OS == LV_OS_NONE, i.e. no internal locking at all) without
 * scattering mutexes through the network and audio code.
 *
 * Posting is always non-blocking. A full queue means the UI is wedged, and in
 * that situation dropping a VU-meter update is strictly better than stalling
 * the WebSocket reader.
 */
#pragma once

#include "app_config.h"

bool bus_begin();

/// Non-blocking post. Returns false if the queue was full (event discarded).
bool bus_post(const UiEvent &ev);

/// Convenience wrappers used by the worker tasks.
bool bus_post_link(LinkState st);
bool bus_post_call_start(uint32_t dmr_id, uint32_t talkgroup, const char *call);
bool bus_post_call_end();
bool bus_post_alias(const char *call, const char *name);
bool bus_post_resolved(uint32_t dmr_id, const char *callsign);
bool bus_post_meter(float db);
bool bus_post_rotary(RotaryCode code);

/// UI-task side. `ticks` may be 0 to poll.
bool bus_recv(UiEvent &ev, TickType_t ticks);
