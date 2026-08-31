/**
 * @file    hose_client.h
 * @brief   BrandMeister Hoseline "spotter" WebSocket client.
 *
 * Owns the network task: brings Wi-Fi up, keeps the WSS session alive, decodes
 * MessagePack frames, feeds u-law straight into the audio jitter buffer and
 * publishes everything else on the UI bus.
 *
 * Everything here runs on one task, so no internal locking is needed; the only
 * cross-task surfaces are the (lock-free) stream buffer and the UI queue.
 */
#pragma once

#include "app_config.h"

bool hose_begin();

/// Change the monitored talkgroup. Safe to call from the UI task: the network
/// task picks the change up and re-subscribes on its next iteration.
void hose_set_talkgroup(uint32_t tg);

/// Current link state, for the banner.
LinkState hose_link_state();

/// Ask for an immediate reconnect (used after editing Wi-Fi credentials).
void hose_request_reconnect();
