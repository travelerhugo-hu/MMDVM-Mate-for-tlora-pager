/**
 * @file    ui.h
 * @brief   LVGL front end: main monitor screen + settings screen.
 *
 * Threading: every function here must be called from the Arduino loop task and
 * nowhere else. LVGL is built with LV_USE_OS == LV_OS_NONE, so it has no
 * internal locking whatsoever; the worker tasks reach the UI only by posting
 * UiEvents (see app_bus.h).
 *
 * Input: MMDVM Mate does *not* use LVGL's keypad/encoder input devices. Both
 * are deleted right after beginLvglHelper() and the raw devices are read
 * directly, for two reasons:
 *   - LilyGoLoRaPager::getRotary() blocks for up to 50 ms on an idle queue, so
 *     letting an LVGL indev poll it caps the whole UI at ~20 fps;
 *   - the screen is a fixed dashboard, not a focus-navigable form, so group
 *     based focus handling would be all cost and no benefit.
 * ui_handle_key() / ui_handle_rotary() therefore receive raw input and decide
 * what it means from the current screen and edit mode.
 */
#pragma once

#include "app_config.h"

/// Build both screens and show the main one.
void ui_begin();

/// Periodic housekeeping: clock, battery, link icon, Wi-Fi scan polling.
/// Call from loop(); internally rate limited, so calling it every pass is fine.
void ui_tick();

/// Apply one event from the worker tasks.
void ui_handle_event(const UiEvent &ev);

/// A key from the TCA8418 matrix. `c` is already the mapped character
/// ('\n' = enter, '\b' = backspace, ' ' = space, otherwise printable).
void ui_handle_key(char c);

void ui_handle_rotary(RotaryCode code);

/// True while the settings screen is up - main.cpp keeps the backlight on and
/// suspends screen blanking while the user is editing.
bool ui_in_settings();

/// True while a text field is being edited, i.e. every printable key belongs
/// to the editor and no global shortcut may fire.
bool ui_capturing_text();
