#pragma once
#include <stdint.h>

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
 * Reports the board's current state to the phone via notification.
 */
void gatt_link_notify_state(uint8_t state);
