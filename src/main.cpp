/**
 * @file    main.cpp
 * @brief   MMDVM Mate - firmware entry point and the single UI task.
 *
 * Task map (everything else is created by the module that owns it):
 *
 *   loopTask   prio 1  core 1   LVGL, keyboard polling, event dispatch  <- here
 *   rotary     prio 3  core 1   blocks on getRotary(), republishes      <- here
 *   audio      prio 6  core 1   u-law decode -> ES8311                  audio_player.cpp
 *   hosenet    prio 4  core 0   TLS + WebSocket + MessagePack           hose_client.cpp
 *   csresolve  prio 2  core 0   radioid.net lookups                     callsign.cpp
 *
 * Only loopTask ever touches LVGL. This build has LV_USE_OS == LV_OS_NONE, so
 * LVGL has no internal locking at all and that invariant is the whole reason
 * the worker tasks talk to the screen through a queue (app_bus.h) instead of
 * calling lv_* directly.
 */

#include <Arduino.h>
#include <LilyGoLib.h>
#include <LV_Helper.h>
#include <lvgl.h>

#include "app_bus.h"
#include "app_config.h"
#include "audio_player.h"
#include "callsign.h"
#include "history.h"
#include "hose_client.h"
#include "settings.h"
#include "ui.h"
#include "wifi_time.h"

// ---------------------------------------------------------------------------
// Hardware we deliberately do not bring up.
//
// MMDVM Mate is a network monitor: the LoRa radio, GNSS, NFC front end, SD
// card and PDM microphone are never used. Skipping them saves ~1.4 s of boot
// time and, more importantly, keeps the SX1262 and the u-blox module powered
// down instead of idling at a few mA each on a 1500 mAh cell.
//
// The RTC, audio codec, keyboard, rotary encoder, fuel gauge and PMU are all
// required and stay enabled.
// ---------------------------------------------------------------------------
static const uint32_t HW_DISABLE_MASK =
    NO_HW_LORA | NO_HW_GPS | NO_HW_NFC | NO_HW_SD | NO_HW_MIC |
    NO_INIT_FATFS | NO_SCAN_I2C_DEV;

// ---------------------------------------------------------------------------
// Screen blanking
// ---------------------------------------------------------------------------
static uint32_t s_last_activity = 0;
static bool     s_screen_off    = false;

/// Only *user* input counts as activity. Incoming traffic deliberately does
/// not wake or hold the screen: on a busy talkgroup that would mean the
/// backlight never goes off, which is the opposite of what the timeout is for.
static inline void note_activity()
{
    s_last_activity = millis();
}

/**
 * @brief  Wake the panel if it is blanked.
 * @return true if this input was consumed by the wake-up, i.e. the caller must
 *         not also act on it. Waking on the same keystroke that opens the
 *         settings menu is a classic way to make a device feel broken.
 */
static bool wake_if_dark()
{
    note_activity();
    if (!s_screen_off) return false;

    s_screen_off = false;
    instance.wakeupDisplay();
    // Full redraw before the backlight comes back, otherwise the first frame
    // after wake-up is whatever was in the panel RAM when we slept.
    lv_obj_invalidate(lv_screen_active());
    lv_timer_handler();
    instance.setBrightness(settings_backlight_level());
    return true;
}

static void service_blanking()
{
    uint32_t timeout = settings_sleep_ms();

    // "Always On", already blanked, or the user is in the middle of editing
    // settings - in all three cases there is nothing to do.
    if (timeout == 0 || s_screen_off || ui_in_settings()) return;
    if ((millis() - s_last_activity) < timeout) return;

    instance.setBrightness(0);
    instance.sleepDisplay();
    s_screen_off = true;
    log_i("display blanked after %lu ms idle", (unsigned long)timeout);
}

// ---------------------------------------------------------------------------
// Rotary encoder reader
//
// LilyGoLoRaPager::getRotary() blocks for up to 50 ms on an empty queue, so it
// cannot live in loop() without pinning the UI at ~20 fps. It also latches the
// centre-button *level* rather than reporting edges, so the press has to be
// differentiated here.
// ---------------------------------------------------------------------------
static void rotary_task(void *)
{
    bool last_btn = false;

    for (;;) {
        RotaryMsg_t msg = instance.getRotary();

        if (msg.dir == ROTARY_DIR_UP) {
            bus_post_rotary(ROT_CW);
        } else if (msg.dir == ROTARY_DIR_DOWN) {
            bus_post_rotary(ROT_CCW);
        }

        if (msg.centerBtnPressed && !last_btn) {
            bus_post_rotary(ROT_CLICK);
        }
        last_btn = msg.centerBtnPressed;
    }
}

// ---------------------------------------------------------------------------
// Input dispatch
// ---------------------------------------------------------------------------
static void poll_keyboard()
{
    char c    = '\0';
    int  state = instance.getKeyChar(&c);

    if (state != KEYBOARD_PRESSED || c == '\0') return;

    if (wake_if_dark()) return;     // this press only turned the screen back on
    ui_handle_key(c);
}

static void drain_event_bus()
{
    UiEvent ev;
    while (bus_recv(ev, 0)) {
        if (ev.type == EV_ROTARY) {
            if (wake_if_dark()) continue;
        }
        ui_handle_event(ev);
    }
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
static void diag_report()
{
    static uint32_t next_at = 0;
    if ((int32_t)(millis() - next_at) < 0) return;
    next_at = millis() + 30000;

    log_i("heap %u free / psram %u free / loop stack %u words left / audio drops %lu",
          (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getFreePsram(),
          (unsigned)uxTaskGetStackHighWaterMark(nullptr),
          (unsigned long)audio_dropped_bytes());
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);
    delay(20);
    log_i("%s %s booting", APP_NAME, APP_VERSION);

    uint32_t probed = instance.begin(HW_DISABLE_MASK);
    log_i("hardware probe mask = 0x%08lx", (unsigned long)probed);

    // Settings first: the backlight level, volume and talkgroup that every
    // later step reads all live in there.
    settings_begin();
    history_begin();

    beginLvglHelper(instance);

    // MMDVM Mate reads the keyboard and the encoder itself (see ui.h). Leaving
    // LVGL's input devices registered would mean the encoder queue is drained
    // twice - each read consumes the event, so roughly every other detent
    // would silently disappear.
    if (lv_indev_t *kb  = lv_get_keyboard_indev()) lv_indev_delete(kb);
    if (lv_indev_t *enc = lv_get_encoder_indev())  lv_indev_delete(enc);
    if (lv_indev_t *tp  = lv_get_touch_indev())    lv_indev_delete(tp);

    // Dark until the first frame is composed, so the user never sees the
    // uninitialised panel contents.
    instance.setBrightness(0);

    if (!bus_begin())   log_e("event bus init failed");
    if (!audio_begin()) log_e("audio init failed - monitor will be silent");
    audio_set_volume(g_settings.volume);

    callsign_begin();

    // Seeds the system clock from the PCF85063 and registers the Wi-Fi/SNTP
    // hooks. Must run before any network task can raise a GOT_IP event.
    wifi_time_boot_from_rtc();

    ui_begin();
    lv_timer_handler();                     // compose frame 1 while still dark
    instance.incrementalBrightness(settings_backlight_level(), 8, false);

    // Network last: hose_begin() owns Wi-Fi association as well, and it starts
    // trying immediately, so everything it can call into must already exist.
    if (!hose_begin()) log_e("network task failed to start");
    hose_set_talkgroup(g_settings.talkgroup);

    BaseType_t rc = xTaskCreatePinnedToCore(rotary_task, "rotary",
                                            TASK_ROT_STACK, nullptr,
                                            TASK_ROT_PRIO, nullptr,
                                            TASK_ROT_CORE);
    if (rc != pdPASS) log_e("rotary task create failed rc=%d", (int)rc);

    note_activity();
    log_i("boot complete, monitoring TG %lu", (unsigned long)g_settings.talkgroup);
}

void loop()
{
    instance.loop();            // RTC / sensor / PMU interrupt fan-out

    poll_keyboard();
    drain_event_bus();

    if (s_screen_off) {
        // Panel is asleep: keep LVGL's timers ticking so animations and the
        // clock label stay coherent, but at a rate that costs nothing. There
        // is no point pushing pixels down the SPI bus to a sleeping ST7796.
        static uint32_t next_tick = 0;
        if ((int32_t)(millis() - next_tick) >= 0) {
            next_tick = millis() + 250;
            ui_tick();
            lv_timer_handler();
        }
        history_maintain();
        diag_report();
        delay(20);
        return;
    }

    ui_tick();
    history_maintain();
    service_blanking();
    diag_report();

    uint32_t idle = lv_timer_handler();
    if (idle == LV_NO_TIMER_READY || idle > 5) idle = 5;
    delay(idle ? idle : 1);
}
