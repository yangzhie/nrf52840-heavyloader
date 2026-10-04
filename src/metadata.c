#include <zephyr/sys/byteorder.h>

#include "metadata.h"

bool metadata_parse(const uint8_t *data, size_t len, struct metadata *out)
{
    // Check: length of the incoming payload
    if (len < METADATA_MIN_LEN) {
        return false;
    }

    // Check: Magic of the payload - special identifier in the periodic advertisement
    if (data[0] != METADATA_MAGIC_0 || data[1] != METADATA_MAGIC_1) {
        return false;
    }

    // Check protocol version
    if (data[2] != METADATA_VERSION) {
        return false;
    }

    // All checks passed, write into metadata 
    out->protocol_version = data[2];
    out->route_id = sys_get_le16(&data[3]); // Both 3 & 4 little endian bytes
    out->stop_index = data[5];
    out->direction = data[6];
    out->language = data[7];
    out->audio_id = data[8];

    return true;
}