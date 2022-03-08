/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"

TEST_CASE("CRC calculations: crc32c", "[crc32c]")
{
    auto crc32c = wiredtiger_crc32c_func();

    REQUIRE(crc32c(nullptr, 0) == 0);
    REQUIRE(crc32c("", 0) == 0);
    REQUIRE(crc32c("non-empty", 0) == 0);

    const std::vector<uint8_t> no_bits_set = {0x00, 0x00, 0x00, 0x00};
    REQUIRE(crc32c(&no_bits_set[0], no_bits_set.size()) == 0x48674bc7);

    const uint32_t zero_point = 0x9BE09BAB;
    REQUIRE(crc32c(&zero_point, sizeof(zero_point)) == 0);

    const std::vector<uint8_t> all_bits_set = {0xff, 0xff, 0xff, 0xff};
    REQUIRE(crc32c(&all_bits_set[0], all_bits_set.size()) == 0xffffffff);

    std::string lots_of_data = "this is a really long string";
    for (int i = 0; i < 10; i++)
        lots_of_data += lots_of_data;
    REQUIRE(crc32c(lots_of_data.c_str(), lots_of_data.length()) == 0x47a00ee5);
}
