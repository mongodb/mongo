/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

// This file unit tests the macros and functions contained in intpack_inline.h.

#include "wt_internal.h"
#include <catch2/catch.hpp>

/*
 * wt_size_check_pack_wrapper --
 *     The WT_SIZE_CHECK_PACK() macro which will directly call return on failure. Creating a wrapper
 *     function thereby ensures that the macro's return call is restricted to this function's scope.
 */
static int
wt_size_check_pack_wrapper(int value, size_t maxValue)
{
    WT_SIZE_CHECK_PACK(value, maxValue);
    return 0;
}

/*
 * wt_size_check_unpack_wrapper --
 *     The WT_SIZE_CHECK_UNPACK() macro which will directly call return on failure. Creating a
 *     wrapper function thereby ensures that the macro's return call is restricted to this
 *     function's scope.
 */
static int
wt_size_check_unpack_wrapper(int value, size_t maxValue)
{
    WT_SIZE_CHECK_UNPACK(value, maxValue);
    return 0;
}

/*
 * wt_leading_zeros_wrapper --
 *     This function wraps WT_LEADING_ZEROS() to create a function that returns the number of
 *     leading zeros, rather than requiring a result variable to be passed in
 */
template <class T>
static int
wt_leading_zeros_wrapper(T value)
{
    int result = 0;
    WT_LEADING_ZEROS(value, result);
    return result;
}

static void
unpack_posint_and_check(std::vector<uint8_t> const &packed, uint64_t expectedValue)
{
    uint8_t const *p = packed.data();
    uint64_t unpackedValue = 0;
    REQUIRE(__wt_vunpack_posint(&p, packed.size(), &unpackedValue) == 0);
    REQUIRE(unpackedValue == expectedValue);
}

static void
unpack_negint_and_check(std::vector<uint8_t> const &packed, uint64_t expectedValue)
{
    uint8_t const *p = packed.data();
    uint64_t unpackedValue = 0;
    REQUIRE(__wt_vunpack_negint(&p, packed.size(), &unpackedValue) == 0);
    REQUIRE(unpackedValue == expectedValue);
}

static void
unpack_int_and_check(std::vector<uint8_t> const &packed, int64_t expectedValue)
{
    uint8_t const *p = packed.data();
    int64_t unpackedValue = 0;
    REQUIRE(__wt_vunpack_int(&p, packed.size(), &unpackedValue) == 0);
    REQUIRE(unpackedValue == expectedValue);
}

static void
test_pack_and_unpack_int(int64_t value, std::vector<uint8_t> const &expectedPacked)
{
    std::vector<uint8_t> packed(expectedPacked.size(), 0);
    uint8_t *p = packed.data();
    REQUIRE(__wt_vpack_int(&p, packed.size(), value) == 0);
    CHECK(packed == expectedPacked);
    unpack_int_and_check(packed, value);
}

TEST_CASE("Integer packing macros: byte min/max", "[intpack]")
{
    /*
     * These macros have no type, so assign macros into variables to give them a type.
     */
    uint16_t neg_1byte_min_16 = NEG_1BYTE_MIN;
    uint16_t neg_2byte_min_16 = NEG_2BYTE_MIN;
    uint16_t pos_1byte_max_16 = POS_1BYTE_MAX;
    uint16_t pos_2byte_max_16 = POS_2BYTE_MAX;

    uint32_t neg_1byte_min_32 = NEG_1BYTE_MIN;
    uint32_t neg_2byte_min_32 = NEG_2BYTE_MIN;
    uint32_t pos_1byte_max_32 = POS_1BYTE_MAX;
    uint32_t pos_2byte_max_32 = POS_2BYTE_MAX;

    uint64_t neg_1byte_min_64 = NEG_1BYTE_MIN;
    uint64_t neg_2byte_min_64 = NEG_2BYTE_MIN;
    uint64_t pos_1byte_max_64 = POS_1BYTE_MAX;
    uint64_t pos_2byte_max_64 = POS_2BYTE_MAX;

    CHECK(neg_1byte_min_16 == 0xffc0u);
    CHECK(neg_2byte_min_16 == 0xdfc0u);
    CHECK(pos_1byte_max_16 == 0x003fu);
    CHECK(pos_2byte_max_16 == 0x203fu);

    CHECK(neg_1byte_min_32 == 0xffffffc0lu);
    CHECK(neg_2byte_min_32 == 0xffffdfc0lu);
    CHECK(pos_1byte_max_32 == 0x0000003flu);
    CHECK(pos_2byte_max_32 == 0x0000203flu);

    CHECK(neg_1byte_min_64 == 0xffffffffffffffc0llu);
    CHECK(neg_2byte_min_64 == 0xffffffffffffdfc0llu);
    CHECK(pos_1byte_max_64 == 0x000000000000003fllu);
    CHECK(pos_2byte_max_64 == 0x000000000000203fllu);
};

TEST_CASE("Integer packing macros: calculations", "[intpack]")
{
    REQUIRE(GET_BITS(0x01ll, 8, 0) == 0x01ll);
    REQUIRE(GET_BITS(0xffll, 8, 0) == 0xffll);
    REQUIRE(GET_BITS(0xffll, 8, 3) == 0x1fll);
    REQUIRE(GET_BITS(0xf0ll, 8, 3) == 0x1ell);
    REQUIRE(GET_BITS(0x8000ll, 8, 0) == 0x00ll);
    REQUIRE(GET_BITS(0x8000ll, 13, 8) == 0x00ll);
    REQUIRE(GET_BITS(0x8000ll, 16, 8) == 0x80ll);
    REQUIRE(GET_BITS(0x8000ll, 16, 15) == 0x1ll);

    CHECK(wt_size_check_pack_wrapper(100, 0) == 0);
    CHECK(wt_size_check_pack_wrapper(100, 256) == 0);
    CHECK(wt_size_check_pack_wrapper(100, 4) == ENOMEM);
    CHECK(wt_size_check_pack_wrapper(300, 8) == ENOMEM);

    CHECK(wt_size_check_unpack_wrapper(100, 0) == 0);
    CHECK(wt_size_check_unpack_wrapper(100, 256) == 0);
    CHECK(wt_size_check_unpack_wrapper(100, 4) == EINVAL);
    CHECK(wt_size_check_unpack_wrapper(300, 8) == EINVAL);

    CHECK(wt_leading_zeros_wrapper<uint64_t>(0) == sizeof(uint64_t));
    CHECK(wt_leading_zeros_wrapper<uint64_t>(0x1) == 7);
    CHECK(wt_leading_zeros_wrapper<uint64_t>(0x100) == 6);
    CHECK(wt_leading_zeros_wrapper<uint64_t>(0x1ff) == 6);
    CHECK(wt_leading_zeros_wrapper<uint64_t>(0x10100) == 5);
    CHECK(wt_leading_zeros_wrapper<uint64_t>(0x101ff) == 5);

    /*
     * WT_LEADING_ZEROS uses sizeof(type) if the value is 0, but assumes uint64_t if non-zero, given
     * odd results
     */
    CHECK(wt_leading_zeros_wrapper<uint8_t>(0) == sizeof(uint8_t));
    CHECK(wt_leading_zeros_wrapper<uint8_t>(0x1) == 7);
    CHECK(wt_leading_zeros_wrapper<uint32_t>(0) == sizeof(uint32_t));
    CHECK(wt_leading_zeros_wrapper<uint32_t>(0x1) == 7);
}

TEST_CASE("Integer packing functions: __wt_vpack_posint and __wt_vunpack_posint", "[intpack]")
{
    std::vector<uint8_t> packed(8, 0);

    SECTION("pack and unpack 7")
    {
        uint8_t *p = packed.data();
        uint64_t value = 7;
        REQUIRE(__wt_vpack_posint(&p, packed.size(), value) == 0);
        REQUIRE(packed[0] == 1);
        REQUIRE(packed[1] == 7);
        REQUIRE(packed[2] == 0);
        REQUIRE(packed[3] == 0);
        REQUIRE(packed[4] == 0);
        REQUIRE(packed[5] == 0);
        REQUIRE(packed[6] == 0);
        REQUIRE(packed[7] == 0);
        unpack_posint_and_check(packed, value);
    }

    SECTION("pack and unpack 42")
    {
        uint8_t *p = packed.data();
        uint64_t value = 42;
        REQUIRE(__wt_vpack_posint(&p, packed.size(), value) == 0);
        REQUIRE(packed[0] == 1);
        REQUIRE(packed[1] == 42);
        REQUIRE(packed[2] == 0);
        REQUIRE(packed[3] == 0);
        REQUIRE(packed[4] == 0);
        REQUIRE(packed[5] == 0);
        REQUIRE(packed[6] == 0);
        REQUIRE(packed[7] == 0);
        unpack_posint_and_check(packed, value);
    }

    SECTION("pack and unpack 0x1234")
    {
        uint8_t *p = packed.data();
        uint64_t value = 0x1234;
        REQUIRE(__wt_vpack_posint(&p, packed.size(), value) == 0);
        REQUIRE(packed[0] == 2);
        REQUIRE(packed[1] == 0x12);
        REQUIRE(packed[2] == 0x34);
        REQUIRE(packed[3] == 0);
        REQUIRE(packed[4] == 0);
        REQUIRE(packed[5] == 0);
        REQUIRE(packed[6] == 0);
        REQUIRE(packed[7] == 0);
        unpack_posint_and_check(packed, value);
    }

    SECTION("pack and unpack 0x123456789")
    {
        uint8_t *p = packed.data();
        uint64_t value = 0x123456789;
        REQUIRE(__wt_vpack_posint(&p, 2, 0x123456789) == ENOMEM);
        REQUIRE(__wt_vpack_posint(&p, packed.size(), 0x123456789) == 0);
        REQUIRE(packed[0] == 5);
        REQUIRE(packed[1] == 0x01);
        REQUIRE(packed[2] == 0x23);
        REQUIRE(packed[3] == 0x45);
        REQUIRE(packed[4] == 0x67);
        REQUIRE(packed[5] == 0x89);
        REQUIRE(packed[6] == 0);
        REQUIRE(packed[7] == 0);
        unpack_posint_and_check(packed, value);
    }
}

TEST_CASE("Integer packing functions: __wt_vpack_negint and __wt_vunpack_negint", "[intpack]")
{
    std::vector<uint8_t> packed(8, 0);

    SECTION("pack and unpack -7")
    {
        uint8_t *p = packed.data();
        uint64_t value = -7;
        REQUIRE(__wt_vpack_negint(&p, packed.size(), value) == 0);
        REQUIRE(packed[0] == 7);    /* 7 leading 0x0ff bytes, if stored as signed 64-bit */
        REQUIRE(packed[1] == 0xf9); /* -7 as a signed 8-bit number stored in one byte */
        REQUIRE(packed[2] == 0);
        REQUIRE(packed[3] == 0);
        REQUIRE(packed[4] == 0);
        REQUIRE(packed[5] == 0);
        REQUIRE(packed[6] == 0);
        REQUIRE(packed[7] == 0);
        unpack_negint_and_check(packed, value);
    }

    SECTION("pack and unpack -42")
    {
        uint8_t *p = packed.data();
        uint64_t value = -42;
        REQUIRE(__wt_vpack_negint(&p, packed.size(), value) == 0);
        REQUIRE(packed[0] == 7);    /* 7 leading 0x0ff bytes, if stored as signed 64-bit */
        REQUIRE(packed[1] == 0xd6); /* -42 as a signed 64-bit number stored in one byte */
        REQUIRE(packed[2] == 0);
        REQUIRE(packed[3] == 0);
        REQUIRE(packed[4] == 0);
        REQUIRE(packed[5] == 0);
        REQUIRE(packed[6] == 0);
        REQUIRE(packed[7] == 0);
        unpack_negint_and_check(packed, value);
    }

    SECTION("pack and unpack -4242")
    {
        uint8_t *p = packed.data();
        uint64_t value = -4242;
        REQUIRE(__wt_vpack_negint(&p, packed.size(), value) == 0);
        REQUIRE(packed[0] == 6);    /* 6 leading 0x0ff bytes, if stored as signed 64-bit */
        REQUIRE(packed[1] == 0xef); /* 1st byte of -4242 as a signed 64-bit number in two bytes */
        REQUIRE(packed[2] == 0x6e); /* 2nd byte of -4242 as a signed 64-bit number in two bytes */
        REQUIRE(packed[3] == 0);
        REQUIRE(packed[4] == 0);
        REQUIRE(packed[5] == 0);
        REQUIRE(packed[6] == 0);
        REQUIRE(packed[7] == 0);
        unpack_negint_and_check(packed, value);
    }
}

TEST_CASE("Integer packing functions: __wt_vpack_int and __wt_vunpack_int", "[intpack]")
{
    /*
     * While the code in each SECTION is small, keeping the code in separate SECTIONS makes it
     * easier to determine which test has failed should any fail.
     */

    SECTION("pack and unpack 7")
    {
        /*
         * Expected result is 0x80     | 0x07    = 0x87
         *                    (marker)  (value)
         */
        test_pack_and_unpack_int(7, {0x87, 0, 0, 0, 0, 0, 0, 0});
    }

    SECTION("pack and unpack 42")
    {
        /*
         * 42 = 0x2a
         *
         * Expected result is 0x80     | 0x2a    = 0x0aa
         *                    (marker)  (value)
         */
        test_pack_and_unpack_int(42, {0xaa, 0, 0, 0, 0, 0, 0, 0});
    }

    SECTION("pack and unpack 256")
    {
        /*
         * 256 = 0x100
         * 256 - (POS_1BYTE_MAX + 1) = 256 - 0x40 = 0x00c0
         *
         * Expected result is 0x0c0   | 0x00                = 0x0c0, and  0x0c0
         *                    (marker)  (top bits of value)               (bottom 8 bits of value)
         */
        test_pack_and_unpack_int(256, {0xc0, 0xc0, 0, 0, 0, 0, 0, 0});
    }

    SECTION("pack and unpack 257")
    {
        /*
         * 257 = 0x101
         * 257 - (POS_1BYTE_MAX + 1) = 257 - 0x40 = 0x00c1
         *
         * Expected result is 0x0c0   | 0x00              = 0x0c0, and  0x0c1
         *                    (marker)  (top bits of value)             (bottom 8 bits of value)
         */
        test_pack_and_unpack_int(257, {0xc0, 0xc1, 0, 0, 0, 0, 0, 0});
    }

    SECTION("pack and unpack 0x1234")
    {
        /*
         * 0x1234 - (POS_1BYTE_MAX + 1) = 0x1234 - 0x40 = 0x11f4
         *
         * Expected result is 0x0c0    | 0x11              = 0x0d1, and  0x0f4
         *                    (marker)  (top bits of value)              (bottom 8 bits of value)
         */
        test_pack_and_unpack_int(0x1234, {0xd1, 0xf4, 0, 0, 0, 0, 0, 0});
    }

    SECTION("pack and unpack 0x123456789 - won't fit")
    {
        std::vector<uint8_t> packed(8, 0);
        uint8_t *p = packed.data();
        int64_t value = 0x123456789;
        /* value won't fit in two bytes */
        REQUIRE(__wt_vpack_int(&p, 2, value) == ENOMEM);
        /* value should fit in 8 bytes */
        REQUIRE(__wt_vpack_int(&p, packed.size(), value) == 0);
    }

    SECTION("pack and unpack 0x123456789 - should fit")
    {
        /*
         * The value that is stored in this case is (0x123456789 - 0x2040) = 0x123454749. For the
         * first byte: 'e' is the marker and '5' is the length in bytes.
         */
        test_pack_and_unpack_int(0x123456789, {0xe5, 0x01, 0x23, 0x45, 0x47, 0x49, 0, 0});
    }

    SECTION("pack and unpack -7")
    {
        /*
         * -7 = 0xffffffffffffffc0
         * -7 - 0xffffffffffffffc0 = 0x39
         *
         * Expected result is 0x40     | 0x39                     = 0x79
         *                    (marker)  (bottom 6 bits of value)
         */
        test_pack_and_unpack_int(-7, {0x79, 0, 0, 0, 0, 0, 0, 0});
    }

    SECTION("pack and unpack -42")
    {
        /*
         * -42 = 0xffffffffffffffd6
         * -42 - 0xffffffffffffffc0 = 0x16
         *
         * Expected result is 0x40     | 0x16                     = 0x56
         *                    (marker)  (bottom 6 bits of value)
         */
        test_pack_and_unpack_int(-42, {0x56, 0, 0, 0, 0, 0, 0, 0});
    }

    SECTION("pack and unpack -256")
    {
        /*
         * -256 = 0x0ffffffffffffff00
         * -256 - 0x0ffffffffffffdfc0 = 0x1f40
         *
         * Expected result is 0x20     | 0x1f                 = 0x3f, and 0x40
         *                    (marker)  (top bits of value)               (bottom 8 bits of value)
         */
        test_pack_and_unpack_int(-256, {0x3f, 0x40, 0, 0, 0, 0, 0, 0});
    }

    SECTION("pack and unpack -257")
    {
        /*
         * -257 = 0xfffffffffffffeff
         * -257 - 0xffffffffffffdfc0 = 0x1f3f
         *
         * Expected result is 0x20     | 0x1f                 = 0x3f, and 0x3f
         *                    (marker)  (top bits of value)               (bottom 8 bits of value)
         */
        test_pack_and_unpack_int(-257, {0x3f, 0x3f, 0, 0, 0, 0, 0, 0});
    }
}
