#pragma once
#include <stdint.h>
#include <stddef.h> 
#include <stdbool.h>

// Metadata definitions
#define METADATA_MAGIC_0 0x41 // "A"
#define METADATA_MAGIC_1 0x55 // "U"
#define METADATA_VERSION 1
#define METADATA_MIN_LEN 9

// Metadata header - receiving from Broadcast Source
struct metadata
{
    uint8_t protocol_version;
    uint16_t route_id;
    uint8_t stop_index;
    uint8_t direction;
    uint8_t language;
    uint8_t audio_id;
};

/**
 * Parses the metadata received from the source's
 * extended advertisement train. 
 * 
 * @param data non-modification pointer to first byte of payload - determines Magic 
 * @param len how many bytes data points at
 * @param out storing the result - parsing sucess + parsed fields
 * 
 * @return whether the parsing succeeded
 */
bool metadata_parse(const uint8_t *data, size_t len, struct metadata *out);