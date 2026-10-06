#include <zephyr/ztest.h>
#include "metadata.h"

/* Payloads as Android and Zephyr deliver them: the length byte, the 0xFF
 * AD type and the company ID are already stripped, so parsing starts at
 * the "AU" magic.
 */
static const uint8_t stop1[] = {0x41, 0x55, 0x01, 0x56, 0x00, 0x01, 0x00, 0x01, 0x01};
static const uint8_t stop2[] = {0x41, 0x55, 0x01, 0x56, 0x00, 0x02, 0x00, 0x01, 0x02};
static const uint8_t stop3[] = {0x41, 0x55, 0x01, 0x56, 0x00, 0x03, 0x00, 0x01, 0x03};
static const uint8_t stop4[] = {0x41, 0x55, 0x01, 0x56, 0x00, 0x04, 0x00, 0x01, 0x04};

ZTEST_SUITE(metadata, NULL, NULL, NULL, NULL, NULL);

ZTEST(metadata, test_decodes_every_field_of_stop_1)
{
	struct metadata m = {0};

	zassert_true(metadata_parse(stop1, sizeof(stop1), &m));

	zassert_equal(m.protocol_version, 1);
	zassert_equal(m.route_id, 86);
	zassert_equal(m.stop_index, 1);
	zassert_equal(m.direction, 0);
	zassert_equal(m.language, 1);
}

ZTEST(metadata, test_decodes_all_four_stops)
{
	struct metadata m = {0};

	zassert_true(metadata_parse(stop1, sizeof(stop1), &m));
	zassert_equal(m.stop_index, 1);

	zassert_true(metadata_parse(stop2, sizeof(stop2), &m));
	zassert_equal(m.stop_index, 2);

	zassert_true(metadata_parse(stop3, sizeof(stop3), &m));
	zassert_equal(m.stop_index, 3);

	zassert_true(metadata_parse(stop4, sizeof(stop4), &m));
	zassert_equal(m.stop_index, 4);
}

ZTEST(metadata, test_route_id_is_little_endian)
{
	struct metadata m = {0};

	/* 0x56 0x00 little-endian is 86. Byte-swapped it would read 22016. */
	zassert_true(metadata_parse(stop1, sizeof(stop1), &m));
	zassert_equal(m.route_id, 86);
}

ZTEST(metadata, test_rejects_short_payload)
{
	struct metadata m = {0};
	const uint8_t truncated[] = {0x41, 0x55, 0x01, 0x56};

	zassert_false(metadata_parse(truncated, sizeof(truncated), &m));
}

ZTEST(metadata, test_rejects_empty_payload)
{
	struct metadata m = {0};

	zassert_false(metadata_parse(NULL, 0, &m));
}

ZTEST(metadata, test_rejects_wrong_magic)
{
	struct metadata m = {0};
	/* "XY" instead of "AU" */
	const uint8_t foreign[] = {0x58, 0x59, 0x01, 0x56, 0x00, 0x01, 0x00, 0x01, 0x01};

	zassert_false(metadata_parse(foreign, sizeof(foreign), &m));
}

ZTEST(metadata, test_rejects_unknown_version)
{
	struct metadata m = {0};
	const uint8_t v2[] = {0x41, 0x55, 0x02, 0x56, 0x00, 0x01, 0x00, 0x01, 0x01};

	zassert_false(metadata_parse(v2, sizeof(v2), &m));
}

ZTEST(metadata, test_accepts_longer_payload)
{
	struct metadata m = {0};
	/* A future version appending fields should still parse at the first 9. */
	const uint8_t extended[] = {0x41, 0x55, 0x01, 0x56, 0x00, 0x02, 0x00,
				    0x01, 0x02, 0xAA, 0xBB};

	zassert_true(metadata_parse(extended, sizeof(extended), &m));
	zassert_equal(m.stop_index, 2);
}