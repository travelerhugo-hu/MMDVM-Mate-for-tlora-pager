#include "wifi_time.h"
#include "settings.h"
#include "app_bus.h"

#include <LilyGoLib.h>
#include <WiFi.h>
#include <esp_sntp.h>

/// Anything before 2021-01-01 means "the clock has never been set".
static const time_t EPOCH_SANITY = 1609459200;

static volatile bool s_time_synced = false;

static void on_sntp_sync(struct timeval *)
{
    s_time_synced = true;
    // Push the freshly disciplined time into the PCF85063 so the next cold
    // boot starts with a correct banner instead of 1970.
    instance.rtc.hwClockWrite();
    log_i("SNTP sync ok, hardware RTC updated");
}

static void on_wifi_event(WiFiEvent_t event)
{
    switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        log_i("Wi-Fi up, IP=%s", WiFi.localIP().toString().c_str());
        bus_post_link(LINK_WIFI_UP);
        // SNTP must be (re)configured after the interface has an address,
        // otherwise the DHCP-supplied server is discarded.
        configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "time.nist.gov");
        break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        log_w("Wi-Fi down");
        bus_post_link(LINK_WIFI_DOWN);
        break;
    default:
        break;
    }
}

void wifi_time_boot_from_rtc()
{
    setenv("TZ", "UTC0", 1);
    tzset();

    // Pull the RTC into the system clock. If the coin cell was flat this just
    // leaves us at the epoch, which utc_now() reports as "not set".
    instance.rtc.hwClockRead();

    time_t now = time(nullptr);
    if (now > EPOCH_SANITY) {
        log_i("clock seeded from hardware RTC: %lu", (unsigned long)now);
    } else {
        log_w("hardware RTC not set, waiting for SNTP");
    }

    sntp_set_time_sync_notification_cb(on_sntp_sync);
    WiFi.onEvent(on_wifi_event);
}

void wifi_begin_connect()
{
    if (g_settings.wifi_ssid[0] == '\0') {
        log_w("no SSID configured");
        bus_post_link(LINK_WIFI_DOWN);
        return;
    }

    WiFi.persistent(false);         // we own the credentials in our own NVS blob
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(true);            // ~40 mA saved; latency is irrelevant here
    WiFi.begin(g_settings.wifi_ssid, g_settings.wifi_pass);
    log_i("associating with '%s'", g_settings.wifi_ssid);
}

void wifi_disconnect()
{
    WiFi.disconnect(true);
    bus_post_link(LINK_WIFI_DOWN);
}

bool wifi_is_connected() { return WiFi.status() == WL_CONNECTED; }
bool time_is_synced()    { return s_time_synced; }

bool utc_now(struct tm &out)
{
    time_t now = time(nullptr);
    if (now < EPOCH_SANITY) return false;
    gmtime_r(&now, &out);
    return true;
}

void utc_clock_string(char *buf, size_t sz)
{
    if (!buf || sz == 0) return;
    struct tm t;
    if (utc_now(t)) {
        snprintf(buf, sz, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        strlcpy(buf, "--:--:--", sz);
    }
}

void utc_date_string(char *buf, size_t sz)
{
    if (!buf || sz == 0) return;
    struct tm t;
    if (utc_now(t)) {
        snprintf(buf, sz, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    } else {
        strlcpy(buf, "----------", sz);
    }
}
