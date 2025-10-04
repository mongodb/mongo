/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"

// Due credit: the cases that hash to zero came from
// http://www.isthe.com/chongo/tech/comp/fnv/#zero-hash
TEST_CASE("Hashing: hash_fnv64", "[fnv]")
{
    const uint64_t fnv1a_64_init = 0xcbf29ce484222325;
    REQUIRE(__wt_hash_fnv64(nullptr, 0) == fnv1a_64_init);
    REQUIRE(__wt_hash_fnv64("", 0) == fnv1a_64_init);

    REQUIRE(__wt_hash_fnv64("a", 1) == 0xaf63dc4c8601ec8c);
    REQUIRE(__wt_hash_fnv64("asdf", 4) == 0x90285684421f9857);

    const std::vector<uint8_t> hash_to_zero = {0xd5, 0x6b, 0xb9, 0x53, 0x42, 0x87, 0x08, 0x36};
    REQUIRE(__wt_hash_fnv64(&hash_to_zero[0], hash_to_zero.size()) == 0x00);

    const std::string ascii_hash_to_zero = "!0IC=VloaY";
    REQUIRE(__wt_hash_fnv64(&ascii_hash_to_zero[0], ascii_hash_to_zero.length()) == 0x00);

    const std::string alphanum_hash_to_zero = "77kepQFQ8Kl";
    REQUIRE(__wt_hash_fnv64(&alphanum_hash_to_zero[0], alphanum_hash_to_zero.length()) == 0x00);

    std::string really_long = "this is a really long string ";
    for (int i = 0; i < 5; i++)
        really_long += really_long;
    REQUIRE(__wt_hash_fnv64(&really_long[0], really_long.length()) == 0x774ab448918be805);
}
