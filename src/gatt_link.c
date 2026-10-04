#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/autoconf.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "gatt_link.h"

// Comes in from Auracast companion over GATT
static uint8_t requested_stop_index = 1; // Hardcoded for now, 0 = nothing requested

// Initialized array of bt_data: discoverable, LE only, no classic Bluetooth
// const = puts it in flash, not ram; static = file-local, live the whole program
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

uint8_t gatt_link_get_requested_stop_index(void)
{
	return requested_stop_index;
}

int gatt_link_init(void) 
{
    int err;
    
    // Start advertising
    err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err != 0) {
        printk("Advertising failed to start (err %d)\n", err);
        return err;
    }

    printk("Advertising as \"%s\"\n", CONFIG_BT_DEVICE_NAME);
    return 0;
}