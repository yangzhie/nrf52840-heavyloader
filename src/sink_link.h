#pragma once

#include <stdbool.h>

#include <zephyr/bluetooth/addr.h>

/**
 * Whether a sink has been remembered from a previous run.
 */
bool sink_link_has_sink(void);

/**
 * Whether the given address is the remembered sink.
 */
bool sink_link_matches(const bt_addr_le_t *addr);

/**
 * Remembers this address as the rider's sink, persisting it to flash.
 */
void sink_link_set(const bt_addr_le_t *addr);
