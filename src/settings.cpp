#include "settings.h"
#include <Preferences.h>

AppSettings g_settings;

static Preferences s_prefs;
static const char *NVS_NS = "mmdvmmate";

static void apply_defaults()
{
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.talkgroup   = 91;   // BM "Worldwide" - always has traffic, good
                                   // first-boot proof that the RX chain works.
    g_settings.volume      = 60;
    g_settings.bl_index    = 3;    // 100 %
    g_settings.sleep_index = 2;    // 3 min
}

/// Clamp anything that came back from flash. NVS contents are attacker-free
/// but *not* corruption-free, and an out-of-range index here would index past
/// BL_STEPS[]/SLEEP_SECONDS[] on the very first UI paint.
static void sanitise()
{
    g_settings.wifi_ssid[WIFI_SSID_MAX - 1] = '\0';
    g_settings.wifi_pass[WIFI_PASS_MAX - 1] = '\0';

    if (g_settings.talkgroup == 0 || g_settings.talkgroup > 99999999UL) {
        g_settings.talkgroup = 91;
    }
    if (g_settings.volume > 100)    g_settings.volume = 100;
    if (g_settings.bl_index > 3)    g_settings.bl_index = 3;
    if (g_settings.sleep_index > 4) g_settings.sleep_index = 2;
}

void settings_begin()
{
    apply_defaults();

    if (!s_prefs.begin(NVS_NS, /*readOnly=*/false)) {
        log_e("NVS open failed, running on defaults");
        return;
    }

    // Stored as one blob: a partial write can never leave the fields
    // inconsistent with each other, which a key-per-field layout can.
    AppSettings tmp;
    size_t got = s_prefs.getBytes("cfg", &tmp, sizeof(tmp));
    if (got == sizeof(tmp)) {
        g_settings = tmp;
        sanitise();
        log_i("settings loaded: TG=%lu vol=%u bl=%u sleep=%u ssid='%s'",
              (unsigned long)g_settings.talkgroup, g_settings.volume,
              g_settings.bl_index, g_settings.sleep_index, g_settings.wifi_ssid);
    } else if (got != 0) {
        log_w("settings blob size mismatch (%u != %u), using defaults",
              (unsigned)got, (unsigned)sizeof(tmp));
    }
}

void settings_save()
{
    sanitise();
    size_t n = s_prefs.putBytes("cfg", &g_settings, sizeof(g_settings));
    if (n != sizeof(g_settings)) {
        log_e("settings save failed (%u bytes written)", (unsigned)n);
    }
}

uint8_t settings_backlight_level()
{
    return BL_STEPS[g_settings.bl_index & 0x03];
}

uint32_t settings_sleep_ms()
{
    return (uint32_t)SLEEP_SECONDS[g_settings.sleep_index % 5] * 1000UL;
}
