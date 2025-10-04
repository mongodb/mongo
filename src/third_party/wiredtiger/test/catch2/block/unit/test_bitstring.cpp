/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"

TEST_CASE("Bitstring macros: __bit_byte", "[bitstring]")
{
    CHECK((__bit_byte(0)) == 0);
    CHECK((__bit_byte(1)) == 0);
    CHECK((__bit_byte(2)) == 0);
    CHECK((__bit_byte(3)) == 0);
    CHECK((__bit_byte(4)) == 0);
    CHECK((__bit_byte(5)) == 0);
    CHECK((__bit_byte(6)) == 0);
    CHECK((__bit_byte(7)) == 0);
    CHECK((__bit_byte(8)) == 1);
    CHECK((__bit_byte(9)) == 1);
    CHECK((__bit_byte(15)) == 1);
    CHECK((__bit_byte(16)) == 2);
}

TEST_CASE("Bitstring macros: __bit_mask", "[bitstring]")
{
    CHECK((__bit_mask(0)) == 1);
    CHECK((__bit_mask(1)) == 2);
    CHECK((__bit_mask(2)) == 4);
    CHECK((__bit_mask(3)) == 8);
    CHECK((__bit_mask(4)) == 16);
    CHECK((__bit_mask(5)) == 32);
    CHECK((__bit_mask(6)) == 64);
    CHECK((__bit_mask(7)) == 128);
    CHECK((__bit_mask(8)) == 1);
    CHECK((__bit_mask(9)) == 2);
    CHECK((__bit_mask(10)) == 4);
    CHECK((__bit_mask(11)) == 8);
    CHECK((__bit_mask(12)) == 16);
    CHECK((__bit_mask(13)) == 32);
    CHECK((__bit_mask(14)) == 64);
    CHECK((__bit_mask(15)) == 128);
    CHECK((__bit_mask(16)) == 1);
    CHECK((__bit_mask(17)) == 2);
}

TEST_CASE("Bitstring macros: __bitstr_size", "[bitstring]")
{
    CHECK((__bitstr_size(0)) == 0);
    CHECK((__bitstr_size(1)) == 1);
    CHECK((__bitstr_size(2)) == 1);
    CHECK((__bitstr_size(3)) == 1);
    CHECK((__bitstr_size(4)) == 1);
    CHECK((__bitstr_size(5)) == 1);
    CHECK((__bitstr_size(6)) == 1);
    CHECK((__bitstr_size(7)) == 1);
    CHECK((__bitstr_size(8)) == 1);
    CHECK((__bitstr_size(9)) == 2);
    CHECK((__bitstr_size(10)) == 2);
    CHECK((__bitstr_size(11)) == 2);
    CHECK((__bitstr_size(12)) == 2);
    CHECK((__bitstr_size(13)) == 2);
    CHECK((__bitstr_size(14)) == 2);
    CHECK((__bitstr_size(15)) == 2);
    CHECK((__bitstr_size(16)) == 2);
    CHECK((__bitstr_size(17)) == 3);
}

TEST_CASE("Bitstring functions: __bit_nset", "[bitstring]")
{
    const int bit_vector_size = 8;
    std::vector<uint8_t> bit_vector(bit_vector_size, 0);

    for (int i = 0; i < bit_vector_size; i++)
        CHECK(bit_vector[i] == 0x00);

    SECTION("Simple test: set first two bytes")
    {
        __bit_nset(bit_vector.data(), 0, 15);
        CHECK(bit_vector[0] == 0xff);
        CHECK(bit_vector[1] == 0xff);
        CHECK(bit_vector[2] == 0x00);
        CHECK(bit_vector[3] == 0x00);
        CHECK(bit_vector[4] == 0x00);
        CHECK(bit_vector[5] == 0x00);
        CHECK(bit_vector[6] == 0x00);
        CHECK(bit_vector[7] == 0x00);
    }

    SECTION("Simple test: set bytes 1 and 2 bytes")
    {
        __bit_nset(bit_vector.data(), 8, 23);
        CHECK(bit_vector[0] == 0x00);
        CHECK(bit_vector[1] == 0xff);
        CHECK(bit_vector[2] == 0xff);
        CHECK(bit_vector[3] == 0x00);
        CHECK(bit_vector[4] == 0x00);
        CHECK(bit_vector[5] == 0x00);
        CHECK(bit_vector[6] == 0x00);
        CHECK(bit_vector[7] == 0x00);
    }

    SECTION("Simple test: set non byte-aligned bit vector")
    {
        __bit_nset(bit_vector.data(), 9, 20);
        CHECK(bit_vector[0] == 0x00);
        CHECK(bit_vector[1] == 0xfe);
        CHECK(bit_vector[2] == 0x1f);
        CHECK(bit_vector[3] == 0x00);
        CHECK(bit_vector[4] == 0x00);
        CHECK(bit_vector[5] == 0x00);
        CHECK(bit_vector[6] == 0x00);
        CHECK(bit_vector[7] == 0x00);
    }

    SECTION("Simple test: first non byte-aligned bit vector")
    {
        __bit_nset(bit_vector.data(), 0, 20);
        CHECK(bit_vector[0] == 0xff);
        CHECK(bit_vector[1] == 0xff);
        CHECK(bit_vector[2] == 0x1f);
        CHECK(bit_vector[3] == 0x00);
        CHECK(bit_vector[4] == 0x00);
        CHECK(bit_vector[5] == 0x00);
        CHECK(bit_vector[6] == 0x00);
        CHECK(bit_vector[7] == 0x00);
    }

    SECTION("Simple test: last non-aligned bit vector")
    {
        __bit_nset(bit_vector.data(), 36, 63);
        CHECK(bit_vector[0] == 0x00);
        CHECK(bit_vector[1] == 0x00);
        CHECK(bit_vector[2] == 0x00);
        CHECK(bit_vector[3] == 0x00);
        CHECK(bit_vector[4] == 0xf0);
        CHECK(bit_vector[5] == 0xff);
        CHECK(bit_vector[6] == 0xff);
        CHECK(bit_vector[7] == 0xff);
    }
}
