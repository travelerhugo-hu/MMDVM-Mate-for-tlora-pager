#include "audio_player.h"

#include <LilyGoLib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

// ---------------------------------------------------------------------------
// G.711 u-law -> 16-bit linear PCM.
//
// Generated from the ITU-T reference expansion
//     t = ((~u & 0x0F) << 3) + 0x84;  t <<= (~u & 0x70) >> 4;
//     s = (~u & 0x80) ? (0x84 - t) : (t - 0x84);
// and spot-checked against the standard anchors 0x00 -> -32124, 0xFF -> 0.
// Kept as a table in .rodata: 512 bytes of flash buys us a branch-free decode,
// which matters because this runs 8000 times a second.
// ---------------------------------------------------------------------------
static const int16_t ULAW_TO_PCM[256] = {
    -32124, -31100, -30076, -29052, -28028, -27004, -25980, -24956,
    -23932, -22908, -21884, -20860, -19836, -18812, -17788, -16764,
    -15996, -15484, -14972, -14460, -13948, -13436, -12924, -12412,
    -11900, -11388, -10876, -10364,  -9852,  -9340,  -8828,  -8316,
     -7932,  -7676,  -7420,  -7164,  -6908,  -6652,  -6396,  -6140,
     -5884,  -5628,  -5372,  -5116,  -4860,  -4604,  -4348,  -4092,
     -3900,  -3772,  -3644,  -3516,  -3388,  -3260,  -3132,  -3004,
     -2876,  -2748,  -2620,  -2492,  -2364,  -2236,  -2108,  -1980,
     -1884,  -1820,  -1756,  -1692,  -1628,  -1564,  -1500,  -1436,
     -1372,  -1308,  -1244,  -1180,  -1116,  -1052,   -988,   -924,
      -876,   -844,   -812,   -780,   -748,   -716,   -684,   -652,
      -620,   -588,   -556,   -524,   -492,   -460,   -428,   -396,
      -372,   -356,   -340,   -324,   -308,   -292,   -276,   -260,
      -244,   -228,   -212,   -196,   -180,   -164,   -148,   -132,
      -120,   -112,   -104,    -96,    -88,    -80,    -72,    -64,
       -56,    -48,    -40,    -32,    -24,    -16,     -8,      0,
     32124,  31100,  30076,  29052,  28028,  27004,  25980,  24956,
     23932,  22908,  21884,  20860,  19836,  18812,  17788,  16764,
     15996,  15484,  14972,  14460,  13948,  13436,  12924,  12412,
     11900,  11388,  10876,  10364,   9852,   9340,   8828,   8316,
      7932,   7676,   7420,   7164,   6908,   6652,   6396,   6140,
      5884,   5628,   5372,   5116,   4860,   4604,   4348,   4092,
      3900,   3772,   3644,   3516,   3388,   3260,   3132,   3004,
      2876,   2748,   2620,   2492,   2364,   2236,   2108,   1980,
      1884,   1820,   1756,   1692,   1628,   1564,   1500,   1436,
      1372,   1308,   1244,   1180,   1116,   1052,    988,    924,
       876,    844,    812,    780,    748,    716,    684,    652,
       620,    588,    556,    524,    492,    460,    428,    396,
       372,    356,    340,    324,    308,    292,    276,    260,
       244,    228,    212,    196,    180,    164,    148,    132,
       120,    112,    104,     96,     88,     80,     72,     64,
        56,     48,     40,     32,     24,     16,      8,      0,
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static StreamBufferHandle_t s_ring      = nullptr;
static TaskHandle_t         s_task      = nullptr;
static volatile bool        s_playing   = false;
static volatile bool        s_flush_req = false;
static volatile uint32_t    s_dropped   = 0;
static volatile uint8_t     s_volume    = 60;
static volatile bool        s_vol_dirty = true;

/// Last sample of the previous block, so linear interpolation is continuous
/// across block boundaries instead of dipping to zero every 40 ms.
static int16_t s_prev_sample = 0;

// Scratch buffers. Static rather than stack because the audio task runs on a
// 4 kB stack and these would eat most of it.
static uint8_t s_ulaw_buf[AUDIO_BLOCK_SAMPLES];
static int16_t s_pcm_buf[AUDIO_BLOCK_SAMPLES * AUDIO_UPSAMPLE];

// ---------------------------------------------------------------------------

size_t audio_push(const uint8_t *ulaw, size_t len)
{
    if (!s_ring || !ulaw || len == 0) return 0;

    // Zero-tick send: the network task must never block on audio.
    size_t sent = xStreamBufferSend(s_ring, ulaw, len, 0);
    if (sent < len) {
        s_dropped += (uint32_t)(len - sent);
    }
    return sent;
}

void audio_flush()
{
    // Reset is only legal when no task is blocked on the buffer, so we ask the
    // consumer to do it at a safe point instead of calling xStreamBufferReset()
    // from whichever task happens to notice the call ended.
    s_flush_req = true;
}

void audio_set_volume(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_volume    = percent;
    s_vol_dirty = true;
}

bool audio_is_playing()      { return s_playing; }
uint32_t audio_dropped_bytes() { return s_dropped; }

// ---------------------------------------------------------------------------

static void audio_task(void *)
{
    const TickType_t idle_delay = pdMS_TO_TICKS(10);
    bool codec_open = false;

    for (;;) {
        if (s_flush_req) {
            s_flush_req = false;
            xStreamBufferReset(s_ring);
            s_playing     = false;
            s_prev_sample = 0;
        }

        if (s_vol_dirty) {
            s_vol_dirty = false;
            instance.codec.setVolume(s_volume);
        }

        size_t avail = xStreamBufferBytesAvailable(s_ring);

        if (!s_playing) {
            // Pre-buffering: wait for enough audio that a 300 ms network hiccup
            // does not turn into a click.
            if (avail < AUDIO_PREBUFFER) {
                vTaskDelay(idle_delay);
                continue;
            }
            if (!codec_open) {
                int rc = instance.codec.open(AUDIO_BITS, AUDIO_CHANNELS, AUDIO_OUT_RATE);
                if (rc != 0) {
                    log_e("codec open failed rc=%d, dropping audio", rc);
                    xStreamBufferReset(s_ring);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    continue;
                }
                codec_open = true;
                instance.codec.setVolume(s_volume);
            }
            s_playing = true;
        }

        if (avail == 0) {
            // Under-run while "playing": go back to buffering rather than
            // feeding I2S silence, which sounds like stuttering.
            s_playing = false;
            if (codec_open) {
                instance.codec.close();
                codec_open = false;
            }
            continue;
        }

        size_t want = avail < AUDIO_BLOCK_SAMPLES ? avail : AUDIO_BLOCK_SAMPLES;
        size_t got  = xStreamBufferReceive(s_ring, s_ulaw_buf, want,
                                           pdMS_TO_TICKS(AUDIO_UNDERRUN_MS));
        if (got == 0) {
            s_playing = false;
            continue;
        }

        // Decode + 2x linear interpolation in one pass.
        int16_t prev = s_prev_sample;
        for (size_t i = 0; i < got; ++i) {
            int16_t cur = ULAW_TO_PCM[s_ulaw_buf[i]];
            // Midpoint first, then the real sample: keeps group delay at half
            // an input sample instead of a full one.
            s_pcm_buf[i * 2]     = (int16_t)(((int32_t)prev + cur) >> 1);
            s_pcm_buf[i * 2 + 1] = cur;
            prev = cur;
        }
        s_prev_sample = prev;

        int written = instance.codec.write((uint8_t *)s_pcm_buf,
                                           got * AUDIO_UPSAMPLE * sizeof(int16_t));
        if (written < 0) {
            log_e("codec write failed rc=%d", written);
            instance.codec.close();
            codec_open = false;
            s_playing  = false;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

bool audio_begin()
{
    // Trigger level 1: we do our own pre-buffer accounting, so the buffer
    // should wake us as soon as *any* data lands.
    s_ring = xStreamBufferCreate(AUDIO_RING_BYTES, 1);
    if (!s_ring) {
        log_e("jitter buffer alloc failed (%d bytes)", AUDIO_RING_BYTES);
        return false;
    }

    BaseType_t rc = xTaskCreatePinnedToCore(audio_task, "audio",
                                            TASK_AUDIO_STACK, nullptr,
                                            TASK_AUDIO_PRIO, &s_task,
                                            TASK_AUDIO_CORE);
    if (rc != pdPASS) {
        log_e("audio task create failed rc=%d", (int)rc);
        vStreamBufferDelete(s_ring);
        s_ring = nullptr;
        return false;
    }
    return true;
}
