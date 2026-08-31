/**
 * @file    callsign.h
 * @brief   Turning a DMR transmission into a human-readable callsign.
 *
 * Two independent sources, because neither is reliable on its own:
 *
 *  1. The DMR Talker Alias (Hoseline message type 21). Fast - it lands within
 *     ~100 ms of CALL_START - but it arrives as *progressive DMR blocks*, so
 *     early frames are half-assembled and full of embedded NULs and repeated
 *     characters. Real captures from hose.brandmeister.network look like:
 *         "PT2ANG Angelo Maximo"      <- good
 *         "KQ4IIV\0I\0I\0V\0..."      <- same call, mid-reassembly
 *         "My Radi\0\0"               <- not a callsign at all
 *     callsign_from_talker_alias() exists purely to reject the middle two.
 *
 *  2. radioid.net, keyed on the numeric DMR ID that CALL_START always carries.
 *     Authoritative, but it is an HTTPS round-trip, so it runs on its own
 *     low-priority task and answers asynchronously via the UI bus.
 *
 * Results from both paths land in the same LRU cache, so a repeat caller is
 * resolved instantly and without network traffic.
 */
#pragma once

#include "app_config.h"

/// Start the background resolver task. Safe to call before Wi-Fi is up.
bool callsign_begin();

/**
 * Extract a plausible amateur callsign (and optional operator name) from a raw
 * Talker Alias payload.
 *
 * @param raw       Bytes as received - may contain NULs, so `len` is required.
 * @param len       Length of `raw`.
 * @param call      Out: NUL-terminated callsign, untouched on failure.
 * @param call_sz   Size of `call`.
 * @param name      Out: NUL-terminated operator name, may end up empty.
 * @param name_sz   Size of `name`.
 * @return true only if a syntactically valid callsign was recovered.
 */
bool callsign_from_talker_alias(const uint8_t *raw, size_t len,
                                char *call, size_t call_sz,
                                char *name, size_t name_sz);

/// Cache lookup only, no network. Returns false if unknown.
bool callsign_cached(uint32_t dmr_id, char *out, size_t out_sz);

/// Insert/refresh a mapping (used by both the TA path and the HTTP path).
void callsign_cache_put(uint32_t dmr_id, const char *call);

/// Ask the resolver task to look `dmr_id` up on radioid.net. No-op if the ID
/// is already cached or a request for it is already queued.
void callsign_request(uint32_t dmr_id);
