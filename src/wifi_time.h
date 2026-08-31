/**
 * @file    wifi_time.h
 * @brief   Wi-Fi association and UTC clock discipline.
 *
 * The whole UI is UTC-only (this is a ham radio tool - everybody logs in UTC),
 * so the timezone is pinned to UTC0 and never exposed as a setting.
 *
 * Clock sources, in order of preference:
 *   1. SNTP, whenever Wi-Fi is up.
 *   2. The PCF85063 battery-backed RTC, read once at boot so the banner shows
 *      a sane time before the network comes up, and re-written every time SNTP
 *      lands so the RTC stays disciplined across power cycles.
 */
#pragma once

#include "app_config.h"
#include <time.h>

/// Seed system time from the hardware RTC. Call once, early in setup().
void wifi_time_boot_from_rtc();

/// Kick off an association attempt using g_settings. Non-blocking: the result
/// arrives as EV_LINK_STATE on the UI bus.
void wifi_begin_connect();

/// Drop the association (used when the user edits credentials).
void wifi_disconnect();

bool wifi_is_connected();

/// True once SNTP has delivered at least one sample this boot.
bool time_is_synced();

/// Fill `out` with the current UTC time. Returns false if the clock has never
/// been set (pre-2021 epoch), so the UI can render "--:--:--" instead of lying.
bool utc_now(struct tm &out);

/// "HH:MM:SS", or "--:--:--" when the clock is not trustworthy yet.
void utc_clock_string(char *buf, size_t sz);

/// "YYYY-MM-DD", or "----------".
void utc_date_string(char *buf, size_t sz);
