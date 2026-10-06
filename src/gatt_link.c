#include <zephyr/autoconf.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "gatt_link.h"

/**
 * Comes in from Auracast companion over GATT.
 * 0 = nothing requested - stop listening.
 */
static uint8_t requested_stop_index = 1;

// Phone has notifications on?
static bool notifications_enabled;

// Storing current state
static enum gatt_link_state current_state = GATT_LINK_IDLE;

static K_SEM_DEFINE(sem_command_received, 0, 1);

/**
 * Names for the GATT DB, which is a flat list
 * of attributes.
 */
// Name of the group - shows up on the app when it scans
static const struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x6c618b36, 0x1ac6, 0x4e8a, 0x9797, 0x6db150ca9c5f));
// Name of characteristics the phone writes to
static const struct bt_uuid_128 command_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x09162189, 0xa913, 0x415d, 0x9d9a, 0x681c6d450517));
// Names state that the board notifies to
static const struct bt_uuid_128 status_uuid = BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x4d35a4c2, 0x464b, 0x4e22, 0xb21d, 0xc9f904c3d094));

/**
 * Advertising payload. Broadcasted continuously to everyone unprompted.
 * Device name and flags.
 * 
 * Initialized array of bt_data: discoverable, LE only, no classic Bluetooth.
 * const = puts it in flash, not ram; static = file-local, live the whole program.
 */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/**
 * Advertising payload. Is the scan response - active scanner can send a scan
 * request after hearing an advertisment, device replies with second payload.
 * Initialized array of bt_data: discoverable, LE only, no classic Bluetooth.
 * const = puts it in flash, not ram; static = file-local, live the whole program.
 */
static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_128_ENCODE(0x6c618b36, 0x1ac6, 0x4e8a, 0x9797, 0x6db150ca9c5f))
};

/**
 * Handles every GATT write to the command characteristic. Validates write, 
 * stores the byte, scan_recv_cb reads it when deciding the correct transmitter.
 *
 * @return bytes consumed
 */
static ssize_t write_command(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *value = buf;

	// Long writes not supported
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len != sizeof(uint8_t)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

    // Get the stop index
	requested_stop_index = value[0];

    // Check: if stop listening
	if (requested_stop_index == 0) {
		printk("Phone requested: stop listening\n");
	} else {
		printk("Phone requested: stop %u\n", requested_stop_index);
	}

	// Command received, unblock thread
	k_sem_give(&sem_command_received);

	return len;
}

/**
 * Called when the phone subscribes to or unsubscribes from status
 * notifications.
 */
static void status_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notifications_enabled = (value == BT_GATT_CCC_NOTIFY);
	printk("Status notifications %s\n", notifications_enabled ? "enabled" : "disabled");
}

/**
 * Builds attribute table, registers it with GATT server at startup.
 */
BT_GATT_SERVICE_DEFINE(auracast_svc,
	BT_GATT_PRIMARY_SERVICE(&service_uuid),

	BT_GATT_CHARACTERISTIC(&command_uuid.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE,
			       NULL, write_command, NULL),

	BT_GATT_CHARACTERISTIC(&status_uuid.uuid,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),

	BT_GATT_CCC(status_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

void gatt_link_drain_commands(void)
{
	k_sem_reset(&sem_command_received);
}

int gatt_link_wait_for_command(k_timeout_t timeout)
{
	return k_sem_take(&sem_command_received, timeout);
}

static void gatt_link_notify_state(enum gatt_link_state state)
{
	if (!notifications_enabled) {
		return;
	}

	uint8_t value = (uint8_t)state;

	/** 
	 * Attribute 4 is the status characteristic's value, counting from
	 * the service declaration at index 0.
	 */
	int err = bt_gatt_notify(NULL, &auracast_svc.attrs[4], &value, sizeof(value));

	// Logging failure
	if (err != 0) {
		printk("Notify failed (err %d)\n", err);
	}
}

void gatt_link_set_state(enum gatt_link_state state)
{
	if (state == current_state) {
		return;
	}

	current_state = state;
	printk("State: %u\n", (unsigned)state);
	gatt_link_notify_state(state);
}

uint8_t gatt_link_get_requested_stop_index(void)
{
	return requested_stop_index;
}


int gatt_link_init(void) 
{
    int err;
    
    // Start advertising
    err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err != 0) {
        printk("Advertising failed to start (err %d)\n", err);
        return err;
    }

    printk("Advertising as \"%s\"\n", CONFIG_BT_DEVICE_NAME);
    return 0;
}
