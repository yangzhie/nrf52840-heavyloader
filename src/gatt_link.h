#pragma once
#include <stdint.h>

// GATT state enum
enum gatt_link_state {
	GATT_LINK_IDLE, // Nothing requested
	GATT_LINK_SCANNING, // Looking for stop's transmitter
	GATT_LINK_CONNECTING, // Add Source sent, waiting for sink sync
	GATT_LINK_RECEIVING, // Sink reports bis_sync non-zero, audio flowing
	GATT_LINK_NO_SINK, // Not connected to sink
	GATT_LINK_FAILED // Scan time out/sync failure
};

/**
 * Starts advertising so the companion app can find and connect to the board.
 *
 * @return 0 on success
 */
int gatt_link_init(void);

/**
 * The stop index the phone has asked for. Not unspecified args.
 *
 * @return stop index, or 0 if nothing has been requested
 */
uint8_t gatt_link_get_requested_stop_index(void);

/**
 * Setter for notification state
 */
void gatt_link_set_state(enum gatt_link_state state);
