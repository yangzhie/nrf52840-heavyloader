#include <string.h>
#include <errno.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>

#include "sink_link.h"

#define SINK_SETTINGS_KEY "auracast/sink"

static bt_addr_le_t stored_addr;
static bool have_stored_addr;

bool sink_link_has_sink(void)
{
	return have_stored_addr;
}

bool sink_link_matches(const bt_addr_le_t *addr)
{
	// Check: does the sink link have an already stored address
	if (!have_stored_addr) {
		return false;
	}

	// Compare the two addresses
	if (bt_addr_le_cmp(&stored_addr, addr) == 0) {
		return true;
	} else {
		return false;
	}
}

void sink_link_set(const bt_addr_le_t *addr)
{
	int err;

	// Create a copy of the incoming address
	bt_addr_le_copy(&stored_addr, addr);
	have_stored_addr = true;

	// Store the new address
	err = settings_save_one(SINK_SETTINGS_KEY, &stored_addr, sizeof(stored_addr));
	if (err != 0) {
		printk("Failed to save sink address (err %d)\n", err);
		return;
	}

	printk("Remembered sink %s\n", bt_addr_le_str(&stored_addr));
}

/**
 * Called by settings during settings_load() for each key.
 */
static int sink_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	ssize_t bytes;

	if (!settings_name_steq(name, "sink", NULL)) {
		return -ENOENT;
	}

	if (len != sizeof(stored_addr)) {
		return -EINVAL;
	}

	bytes = read_cb(cb_arg, &stored_addr, sizeof(stored_addr));
	if (bytes < 0) {
		return bytes;
	}

	have_stored_addr = true;
	printk("Loaded sink %s\n", bt_addr_le_str(&stored_addr));

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(auracast_sink, "auracast", NULL, sink_settings_set, NULL, NULL);