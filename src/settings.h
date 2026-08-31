/**
 * @file    settings.h
 * @brief   Persistent user settings (NVS-backed) for MMDVM Mate.
 *
 * All settings live in a single POD struct so the UI can read fields without
 * locking. Only the UI task writes them, and writes are always followed by an
 * explicit settings_save() - there is no lazy/auto flush, because an unclean
 * power-down on a battery device is the normal case, not the exception.
 */
#pragma once

#include "app_config.h"

struct AppSettings {
    char     wifi_ssid[WIFI_SSID_MAX];
    char     wifi_pass[WIFI_PASS_MAX];
    uint32_t talkgroup;     ///< BrandMeister talkgroup currently monitored
    uint8_t  volume;        ///< 0..100 %
    uint8_t  bl_index;      ///< index into BL_STEPS[] (0..3 == 25/50/75/100 %)
    uint8_t  sleep_index;   ///< index into SLEEP_SECONDS[] (0..4)
};

/// Global, single instance. Read freely; mutate only from the UI task.
extern AppSettings g_settings;

/// Load from NVS, applying sane defaults for anything missing or corrupt.
void settings_begin();

/// Flush the whole struct back to NVS. Cheap enough to call on every change.
void settings_save();

/// Convenience: current backlight hardware level (0..16) for g_settings.
uint8_t settings_backlight_level();

/// Convenience: current blank timeout in ms, 0 == never blank.
uint32_t settings_sleep_ms();
