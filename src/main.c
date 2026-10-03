/*
 * Copyright (c) 2024 Demant A/S
 * Copyright (c) 2024-2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include <zephyr/autoconf.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/assigned_numbers.h>
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/bap.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/bluetooth/iso.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/types.h>
#include <zephyr/settings/settings.h>

// Definitions
#define NAME_LEN 30 // 30 bytes, size of name buffers
#define SEM_TIMEOUT K_SECONDS(10) // Semaphore for threads

// Temp. struct for one advertisement
struct scan_recv_info {
	char bt_name[NAME_LEN]; // Device local name
	char broadcast_name[NAME_LEN]; // Auracast name 
	uint32_t broadcast_id; // 24-bit Auracast Broadcast ID
	bool has_bass; // BASS flag
	bool has_pacs; // PACS flag
};

// Link with broadcast sink
static struct bt_conn *broadcast_sink_conn;

// Source scan findings - callbacks fill, main adds to source
static uint8_t remote_recv_state_count; // How many receive states the sink exposes
static uint32_t selected_broadcast_id;
static uint8_t selected_sid;
static uint16_t selected_pa_interval;
static bt_addr_le_t selected_addr;

// True when scanning for a source, false when scanning for a sink
static bool scanning_for_broadcast_source;

// Semaphores for discovery, connection and disconnections
static K_SEM_DEFINE(sem_source_discovered, 0, 1);
static K_SEM_DEFINE(sem_sink_discovered, 0, 1);
static K_SEM_DEFINE(sem_sink_connected, 0, 1);
static K_SEM_DEFINE(sem_sink_disconnected, 0, 1);
static K_SEM_DEFINE(sem_security_updated, 0, 1);
static K_SEM_DEFINE(sem_bass_discovered, 0, 1);
static K_SEM_DEFINE(sem_recv_state_read, 0, 1);

/**
 * Callback for bt_data_parse. Handles one element from the chain
 * of AD elements.
 * 
 * UUID list: packed array of 2-byte UUIDs. Names only.
 * e.g. 05(length) 03(type) 4F18(BASS) 4418(VCS)
 * 
 * Service data: UUID + bytes
 * e.g. 06(length) 16(type) 5218(UUID 0x1852 (Broadcast Audio Announcement) 63(Broadcast ID) 1D A2
 * 
 * @param data current advertising data element
 * @param user_data caller data e.g. addr of scan_recv_info
 * 
 * @return keeps parsing onto next AD element if true
 */
static bool device_found(struct bt_data *data, void *user_data)
{
	// Cast caller's type
	struct scan_recv_info *sr_info = (struct scan_recv_info *)user_data;
	struct bt_uuid_16 adv_uuid;

	// Branching on the AD type bytes
	switch (data->type) {
	case BT_DATA_NAME_SHORTENED: // Truncated device name
	case BT_DATA_NAME_COMPLETE: // Full device name
		// Copy the name, or 29 bytes whichever one is smallest
		memcpy(sr_info->bt_name, data->data, MIN(data->data_len, NAME_LEN - 1));
		return true;
	case BT_DATA_BROADCAST_NAME: // Auracast broadcast name
		// Auracast's own name field
		memcpy(sr_info->broadcast_name, data->data, MIN(data->data_len, NAME_LEN - 1));
		return true;
	case BT_DATA_SVC_DATA16: // Type 0x16: see if element has BASS or PACS
		// UUID needs to be aleast 16-bits
		if (data->data_len < BT_UUID_SIZE_16) {
			return true;
		}

		// Turn the 2 raw bytes to UUID obj
		if (!bt_uuid_create(&adv_uuid.uuid, data->data, BT_UUID_SIZE_16)) {
			return true;
		}

		// Check: element has BASS
		if (bt_uuid_cmp(&adv_uuid.uuid, BT_UUID_BASS) == 0) {
			sr_info->has_bass = true;
			return true;
		}

		// Check: element has PACS
		// TODO: deprecate
		if (bt_uuid_cmp(&adv_uuid.uuid, BT_UUID_PACS) == 0) {
			sr_info->has_pacs = true;
			return true;
		}

		// Check: is it a Broadcast Audio Announcement?
		if (bt_uuid_cmp(&adv_uuid.uuid, BT_UUID_BROADCAST_AUDIO) != 0) {
			return true;
		}

		// Broadcast Audio Announcement carries a 24-bit Broadcast ID
		if (data->data_len < BT_UUID_SIZE_16 + BT_AUDIO_BROADCAST_ID_SIZE) {
			return true;
		}

		sr_info->broadcast_id = sys_get_le24(data->data + BT_UUID_SIZE_16);
		return true;
	case BT_DATA_UUID16_SOME: // Partial/full list of 16-bit service UUIDs
	case BT_DATA_UUID16_ALL: // Type 0x03: see if element has BASS or PACS
		// Odd length = malformed
		if (data->data_len % sizeof(uint16_t) != 0U) {
			printk("UUID16 AD malformed\n");
			return true;
		}

		// 2 bytes at a time
		for (size_t i = 0; i < data->data_len; i += sizeof(uint16_t)) {
			const struct bt_uuid *uuid;
			uint16_t u16;

			memcpy(&u16, &data->data[i], sizeof(u16));
			uuid = BT_UUID_DECLARE_16(sys_le16_to_cpu(u16));

			if (bt_uuid_cmp(uuid, BT_UUID_BASS) == 0) {
				sr_info->has_bass = true;
				continue;
			}

			if (bt_uuid_cmp(uuid, BT_UUID_PACS) == 0) {
				sr_info->has_pacs = true;
				continue;
			}
		}
		return true;
	default:
		return true;
	}
}

/**
 * Substring checker.
 * 
 * @param substr child string to be checked
 * @param str parent string
 * 
 * @return true if original string contains child string 
 */
static bool is_substring(const char *substr, const char *str)
{
	// Length of both strings
	const size_t str_len = strlen(str);
	const size_t sub_str_len = strlen(substr);

	// Check: sub-string length is bigger
	if (sub_str_len > str_len) {
		return false;
	}

	// Slide a window along the original string
	for (size_t pos = 0; pos < str_len; pos++) {
		// Check: sub-string exceeds original string
		if (pos + sub_str_len > str_len) {
			return false;
		}

		// Compare
		if (strncasecmp(substr, &str[pos], sub_str_len) == 0) {
			return true;
		}
	}

	return false;
}

/**
 * Zephyr calls when radio picks up an advertisment.
 * 
 * @param info metadata about reception (sender address, RSSI, advertising SID, etc.).
 * Not contents, how to arrived.
 * @param ad Raw advertising payload. A buffer to give to bt_data_parse to walk elements.
 */
static void scan_recv_cb(const struct bt_le_scan_recv_info *info, struct net_buf_simple *ad)
{
	int err;
	struct scan_recv_info sr_info = {0};

	if (scanning_for_broadcast_source) { // Already scanning for source
		// Scan for and select Broadcast Source
		sr_info.broadcast_id = BT_BAP_INVALID_BROADCAST_ID;

		// Only interested in non-connectable periodic advertisers
		if ((info->adv_props & BT_GAP_ADV_PROP_CONNECTABLE) != 0 || info->interval == 0) {
			return;
		}

		// Walk and parse the advertisment, and sr_info stores
		bt_data_parse(ad, device_found, (void *)&sr_info);

		// Check: parsing found a Broadcast Audio Announcement
		if (sr_info.broadcast_id != BT_BAP_INVALID_BROADCAST_ID) {
			printk("Broadcast Source Found:\n");
			printk("BT Name: %s\n", sr_info.bt_name);
			printk("Broadcast Name: %s\n", sr_info.broadcast_name);
			printk("Broadcast ID: 0x%06x\n\n", sr_info.broadcast_id);

			// Stop scanning, no data more is needed
			err = bt_le_scan_stop();
			if (err != 0) {
				printk("bt_le_scan_stop failed with %d\n", err);
			}

			printk("Selecting Broadcast ID: 0x%06x\n", sr_info.broadcast_id);

			// Store the data
			selected_broadcast_id = sr_info.broadcast_id;
			selected_sid = info->sid;
			selected_pa_interval = info->interval;
			bt_addr_le_copy(&selected_addr, info->addr);

			// Hand-off to main function
			k_sem_give(&sem_source_discovered);
		}
	} else { // Not already scanning for source
		// Scan for and connect to Broadcast Sink

		// Only interested in connectable advertisers
		// Sink doesn't advertise interval/periodic train
		if ((info->adv_props & BT_GAP_ADV_PROP_CONNECTABLE) == 0) {
			return;
		}

		// Walk and parse the advertisment, and sr_info stores
		bt_data_parse(ad, device_found, (void *)&sr_info);

		// Check: BASS is found
		if (sr_info.has_bass) {
			printk("Broadcast Sink Found:\n");
			printk("BT Name: %s\n", sr_info.bt_name);

			// Stop scanning, no data more is needed
			err = bt_le_scan_stop();
			if (err != 0) {
				printk("bt_le_scan_stop failed with %d\n", err);
			}

			printk("Connecting to Broadcast Sink: %s\n", sr_info.bt_name);

			// Who to connect to, creation params, connection interval + latency, write conn handle
			err = bt_conn_le_create(info->addr, BT_CONN_LE_CREATE_CONN, BT_BAP_CONN_PARAM_RELAXED, &broadcast_sink_conn);
			
			// Check: failure, restart scanning
			if (err != 0) {
				printk("Failed creating connection (err=%u)\n", err);
				return;
			}

			k_sem_give(&sem_sink_discovered);
		}
	}
}

/**
 * Fires when a scan ends - duration expired.
 */ 
static void scan_timeout_cb(void)
{
	printk("Scan timeout\n");
}

/**
 * vtable - struct of function pointers.
 */ 
static struct bt_le_scan_cb scan_callbacks = {
	.recv = scan_recv_cb,
	.timeout = scan_timeout_cb,
};

/**
 * Scanning for a broadcast source.
 */
static void scan_for_broadcast_source(void)
{
	int err;

	// Set flag
	scanning_for_broadcast_source = true;

	// Start scanning
	err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, NULL);
	if (err) {
		printk("Scanning failed to start (err %d)\n", err);
		return;
	}

	printk("Scanning for Broadcast Source successfully started\n");

	// Block thread until scan_recv_cb gives semaphore
	err = k_sem_take(&sem_source_discovered, K_FOREVER);
	__ASSERT_NO_MSG(err == 0);
}

/**
 * Scanning for a broadcast sink.
 */
static void scan_for_broadcast_sink(void)
{
	int err;

	// Set flag for source to false, cannot do at same time
	scanning_for_broadcast_source = false;

	// Start scanning
	err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, NULL);
	if (err) {
		printk("Scanning failed to start (err %d)\n", err);
		return;
	}

	printk("Scanning for Broadcast Sink successfully started\n");

	// Halt thread until sink is discovered
	err = k_sem_take(&sem_sink_discovered, K_FOREVER);
	__ASSERT_NO_MSG(err == 0);
}

/**
 * Fires when connection attempt finishes.
 * Could be successful/unsuccessful.
 * 
 * @param conn Bluetooth connection
 * @param err error passed
 */
static void connected(struct bt_conn *conn, uint8_t err)
{
	// CB fires for every connection the stack handles
	if (conn != broadcast_sink_conn) {
		return;
	}

	// Check: failure path
	if (err != 0) {
		printk("Failed to connect to %s %u %s\n", bt_conn_dst_str(conn), err, bt_hci_err_to_str(err));

		bt_conn_unref(broadcast_sink_conn);
		broadcast_sink_conn = NULL;

		return;
	}

	// Success, set connected semaphore
	printk("Connected: %s\n", bt_conn_dst_str(conn));
	k_sem_give(&sem_sink_connected);
}

/**
 * Fires when established connection drops.
 * 
 * @param conn Bluetooth connection
 * @param reason HCI reason code
 */
static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	// Check: sink is connected
	if (conn != broadcast_sink_conn) {
		return;
	}

	printk("Disconnected: %s, reason 0x%02x %s\n", bt_conn_dst_str(conn), reason, bt_hci_err_to_str(reason));

	// Release reference and clear handle
	bt_conn_unref(broadcast_sink_conn);
	broadcast_sink_conn = NULL;

	// Wake whoever is waiting
	k_sem_give(&sem_sink_disconnected);
}

/**
 * Fires when security level is changed.
 * 
 * @param conn Bluetooth connection
 * @param level security level
 * @param err security error
 */
static void security_changed_cb(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	// Success, security level changed
	if (err == 0) {
		printk("Security level changed: %u\n", level);
		k_sem_give(&sem_security_updated);
	} else { // Failure
		printk("Failed to set security level: %s(%u)\n", bt_security_err_to_str(err), err);
	}
}

/**
 * When bt_bap_broadcast_assistant_discover finishes walking
 * sink's BASS service. 
 * 
 * @param conn Bluetooth connection
 * @param err error
 * @param recv_state_count how many states the sink handles
 */
static void bap_broadcast_assistant_discover_cb(struct bt_conn *conn, int err, uint8_t recv_state_count)
{
	if (err == 0) {
		printk("BASS discover done with %u recv states\n", recv_state_count);
		remote_recv_state_count = recv_state_count;
		k_sem_give(&sem_bass_discovered);
	} else {
		printk("BASS discover failed (%d)\n", err);
	}
}

/**
 * When bt_bap_broadcast_assistant_add_src adds a source
 * via sink's BASS service. 
 * 
 * @param conn Bluetooth connection
 * @param err error
 */
static void bap_broadcast_assistant_add_src_cb(struct bt_conn *conn, int err)
{
	if (err == 0) {
		printk("BASS add source successful\n");
	} else {
		printk("BASS add source failed (%d)\n", err);
	}
}

/**
 * Tells whether anything actually worked.
 * 
 * Fires via read_recv_states() and when sink tells you.
 * 
 * @param conn Bluetooth connection
 * @param err error
 * @param state 
 */
static void bap_broadcast_assistant_recv_state_read_cb(struct bt_conn *conn, int err, const struct bt_bap_scan_delegator_recv_state *state)
{
	// Check: error
	if (err != 0) {
		printk("BASS recv state read failed (%d)\n", err);
		return;
	}

	// State table is non-empty, can connect to sink
	if (state != NULL) {
		printk("BASS recv state: src_id %u, addr %s, sid %u, sync_state %u, encrypt_state "
		       "%u, num_subgroups %u\n", state->src_id, bt_addr_le_str(&state->addr),
		       state->adv_sid, state->pa_sync_state, state->encrypt_state,
		       state->num_subgroups);

		for (uint8_t i = 0; i < state->num_subgroups; i++) {
			const struct bt_bap_bass_subgroup *subgroup = &state->subgroups[i];

			printk("\t[%d]: BIS sync %u, metadata_len %u\n", i, subgroup->bis_sync, subgroup->metadata_len);
		}
	}

	k_sem_give(&sem_recv_state_read);
}

/** 
 * Callback struct/vtable 
 * 
 * Struct of function pointers - . holds address of function, not result
 * .field = value sets named members and leaves the rest zero
 */
static struct bt_bap_broadcast_assistant_cb ba_cbs = {
	.discover = bap_broadcast_assistant_discover_cb,
	.add_src = bap_broadcast_assistant_add_src_cb,
	.recv_state = bap_broadcast_assistant_recv_state_read_cb,
};

/**
 * Reset application state for next attempt
 */ 
static void reset(void)
{
	int err;

	printk("\n\nResetting...\n\n");

	if (broadcast_sink_conn != NULL) {
		err = bt_conn_disconnect(broadcast_sink_conn, BT_HCI_ERR_LOCALHOST_TERM_CONN);

		if (err != 0) {
			printk("bt_conn_disconnect failed with %d\n", err);
		} else {
			if (k_sem_take(&sem_sink_disconnected, SEM_TIMEOUT) != 0) {
				printk("Timed out waiting for disconnect\n");
			}
		}
	}

	/* Ignore return value as scanning may already be stopped */
	(void)bt_le_scan_stop();

	selected_broadcast_id = BT_BAP_INVALID_BROADCAST_ID;
	selected_sid = 0;
	selected_pa_interval = 0;
	(void)memset(&selected_addr, 0, sizeof(selected_addr));

	k_sem_reset(&sem_source_discovered);
	k_sem_reset(&sem_sink_discovered);
	k_sem_reset(&sem_sink_connected);
	k_sem_reset(&sem_sink_disconnected);
	k_sem_reset(&sem_security_updated);
	k_sem_reset(&sem_bass_discovered);
	k_sem_reset(&sem_recv_state_read);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed_cb
};

/**
 * Read all found receive states - some or all may be empty
 */
static int read_recv_states(void)
{
	for (uint8_t i = 0U; i < remote_recv_state_count; i++) {
		int err;

		err = bt_bap_broadcast_assistant_read_recv_state(broadcast_sink_conn, i);

		if (err != 0) {
			printk("Failed to read receive state[%u]: %d\n", i, err);

			return err;
		}

		err = k_sem_take(&sem_recv_state_read, SEM_TIMEOUT);
		if (err != 0) {
			printk("Failed to take sem_recv_state_read: %d\n", err);

			return err;
		}
	}

	return 0;
}

/**
 * Main driver
 */
int main(void)
{
	int err;

	// Start Bluetooth stack
	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	// Load config
	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	printk("Bluetooth initialized\n");

	// Registers the two callback structs with Zephyr
	bt_bap_broadcast_assistant_register_cb(&ba_cbs);
	bt_le_scan_cb_register(&scan_callbacks);

	while (true) {
		// Fresh structs 
		struct bt_bap_broadcast_assistant_add_src_param param = {0};
		struct bt_bap_bass_subgroup subgroup = {0};

		// Kill anything from prev. attempt
		reset();

		// Sink scan + connection *underway*, not connected
		scan_for_broadcast_sink();

		// Wait for sink to connect
		err = k_sem_take(&sem_sink_connected, SEM_TIMEOUT);
		if (err != 0) {
			printk("Failed to take sem_sink_connected (err %d)\n", err);
			continue;
		}

		// BASS discovery - start GATT service, return instantly
		err = bt_bap_broadcast_assistant_discover(broadcast_sink_conn);
		if (err != 0) {
			printk("Failed to discover BASS on the sink (err %d)\n", err);
			continue;
		}

		// Setting security level
		// BASS requires an encrypted link FIRST
		err = k_sem_take(&sem_security_updated, SEM_TIMEOUT);
		if (err != 0) {
			printk("Failed to take sem_security_updated (err %d)\n", err);
			continue;
		}

		// BASS discovered
		err = k_sem_take(&sem_bass_discovered, SEM_TIMEOUT);
		if (err != 0) {
			printk("Failed to take sem_bass_discovered (err %d)\n", err);
			continue;
		}

		// Read states of sink - informational only
		err = read_recv_states();
		if (err != 0) {
			printk("Failed to read receive states\n");
			continue;
		}

		// Scan for source - block until found
		scan_for_broadcast_source();

		printk("Selected source: id 0x%06X, sid %u, interval %u, addr %s\n", selected_broadcast_id, selected_sid, selected_pa_interval, bt_addr_le_str(&selected_addr));

		/* 
		 * Assistant does not sync to the periodic advertising train
		 * itself. Instead the sink is told to sync (pa_sync = true) and
		 * to choose its own BIS.
		 */
		// Build the Add Source
		param.addr = selected_addr;
		param.adv_sid = selected_sid;
		param.broadcast_id = selected_broadcast_id;
		param.pa_interval = selected_pa_interval;
		param.pa_sync = true;

		// Let sink read the BASE and pick own BIS channels
		subgroup.bis_sync = BT_BAP_BIS_SYNC_NO_PREF;
		subgroup.metadata_len = 0;

		param.num_subgroups = 1;
		param.subgroups = &subgroup;

		printk("Adding source to the sink\n");

		// Add Source - GATT writes to BASS control point
		err = bt_bap_broadcast_assistant_add_src(broadcast_sink_conn, &param);
		if (err) {
			printk("Failed to add source (err %d)\n", err);
			continue;
		}

		printk("Add Source sent — watch the receive state callback\n");

		k_sleep(K_FOREVER);
	}

	return 0;
}