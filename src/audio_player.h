/**
 * @file    audio_player.h
 * @brief   G.711 u-law -> ES8311 playback chain with a jitter buffer.
 *
 * Topology:
 *
 *   [net task] --audio_push()--> StreamBuffer (u-law bytes) --> [audio task]
 *                                                                    |
 *                                              decode + 2x upsample  |
 *                                                                    v
 *                                                        instance.codec.write()
 *
 * The StreamBuffer is a FreeRTOS single-producer/single-consumer primitive, so
 * the hot path needs no mutex at all. The producer never blocks: if the ring
 * is full we drop the frame and count it, because stalling the WebSocket task
 * would back up TCP and make the *next* second of audio worse, not better.
 */
#pragma once

#include "app_config.h"

/// Create the ring buffer and start the playback task. Returns false if either
/// allocation fails - the caller must treat that as fatal for audio.
bool audio_begin();

/// Queue u-law bytes straight off the wire. Never blocks. Returns the number
/// of bytes actually accepted (< len means the jitter buffer overflowed).
size_t audio_push(const uint8_t *ulaw, size_t len);

/// Drop everything queued and go back to pre-buffering. Call on CALL_END and
/// whenever the talkgroup changes, so the tail of an old call cannot bleed
/// into the next one.
void audio_flush();

/// 0..100 %. Applied to the ES8311 output volume; 0 mutes the PA.
void audio_set_volume(uint8_t percent);

/// True while the task is actively feeding I2S (i.e. not pre-buffering).
bool audio_is_playing();

/// Diagnostics: bytes dropped because the jitter buffer was full.
uint32_t audio_dropped_bytes();
