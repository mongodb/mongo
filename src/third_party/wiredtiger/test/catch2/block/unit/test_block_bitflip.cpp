/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * [block_bitflip]: block_read.c
 * This file unit tests the bitflip detection function used to diagnose single-bit memory
 * corruption in block checksums.
 */

#include <catch2/catch.hpp>
#include <cstring>
#include <vector>

#include "wt_internal.h"

/* Fixture to initialize the checksum function needed by __wt_checksum and __wt_checksum_match. */
struct checksum_fixture {
    checksum_fixture()
    {
        // Initialize the checksum function if not already initialized.
        if (__wt_process.checksum == nullptr)
            __wt_process.checksum = wiredtiger_crc32c_func();
    }
};

TEST_CASE("Block bitflip detection: basic functionality", "[block_bitflip]")
{
    checksum_fixture fixture;
    std::vector<uint8_t> data(128, 0);
    size_t bit_position = 0;

    SECTION("Detect single bit flip in first byte")
    {
        // Initialize with some data.
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<uint8_t>(i);

        // Calculate the correct checksum.
        uint32_t correct_checksum = __wt_checksum(data.data(), data.size());

        // Flip the first bit (bit 0 of byte 0).
        data[0] ^= 1;

        // The function should find that flipping bit 0 would match the correct_checksum.
        bool found =
          __ut_block_bitflip_detect(data.data(), data.size(), correct_checksum, &bit_position);

        CHECK(found == true);
        CHECK(bit_position == 0);
    }

    SECTION("Detect single bit flip in middle byte")
    {
        // Initialize with some data.
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<uint8_t>(i * 3 + 42);

        // Calculate the correct checksum.
        uint32_t correct_checksum = __wt_checksum(data.data(), data.size());

        // Flip bit 3 of byte 64.
        size_t target_byte = 64;
        size_t target_bit = 3;
        data[target_byte] ^= (1U << target_bit);

        // The function should find the flipped bit.
        bool found =
          __ut_block_bitflip_detect(data.data(), data.size(), correct_checksum, &bit_position);

        CHECK(found == true);
        CHECK(bit_position == target_byte * 8 + target_bit);
    }

    SECTION("Detect single bit flip in last byte")
    {
        // Initialize with some data.
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<uint8_t>(0xFF - i);

        // Calculate the correct checksum.
        uint32_t correct_checksum = __wt_checksum(data.data(), data.size());

        // Flip the last bit (bit 7 of last byte).
        size_t target_byte = data.size() - 1;
        size_t target_bit = 7;
        data[target_byte] ^= (1U << target_bit);

        // The function should find the flipped bit.
        bool found =
          __ut_block_bitflip_detect(data.data(), data.size(), correct_checksum, &bit_position);

        CHECK(found == true);
        CHECK(bit_position == target_byte * 8 + target_bit);
    }
}

TEST_CASE("Block bitflip detection: no bit flip", "[block_bitflip]")
{
    checksum_fixture fixture;
    std::vector<uint8_t> data(64, 0);
    size_t bit_position = 0;

    SECTION("Checksums match - no corruption")
    {
        // Initialize with some data.
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<uint8_t>(i * 7);

        // Calculate checksum.
        uint32_t checksum = __wt_checksum(data.data(), data.size());

        // Pass the same checksum - should not find any bit flip.
        bool found = __ut_block_bitflip_detect(data.data(), data.size(), checksum, &bit_position);

        CHECK(found == false);
    }

    SECTION("Multiple bits flipped - not detectable")
    {
        // Initialize with some data.
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<uint8_t>(i);

        // Calculate the correct checksum.
        uint32_t correct_checksum = __wt_checksum(data.data(), data.size());

        // Flip two bits.
        data[10] ^= 1;
        data[20] ^= 2;

        // The function should not find a single bit flip that matches
        // (unless by extreme coincidence two single-bit flips result in the same checksum).
        bool found =
          __ut_block_bitflip_detect(data.data(), data.size(), correct_checksum, &bit_position);

        // Most likely this will be false, but checksums can collide.
        // We mainly want to ensure the function doesn't crash.
        (void)found;
    }
}

TEST_CASE("Block bitflip detection: size limits", "[block_bitflip]")
{
    checksum_fixture fixture;
    size_t bit_position = 0;

    SECTION("Block size at limit")
    {
        size_t size = WT_BITFLIP_MAX_SIZE;
        std::vector<uint8_t> data(size, 0);

        // Initialize with pattern.
        for (size_t i = 0; i < size; ++i)
            data[i] = static_cast<uint8_t>(i & 0xFF);

        uint32_t correct_checksum = __wt_checksum(data.data(), data.size());

        // Flip a bit near the beginning.
        data[100] ^= 4;

        // Should still work at exactly the limit.
        bool found =
          __ut_block_bitflip_detect(data.data(), data.size(), correct_checksum, &bit_position);

        CHECK(found == true);
        CHECK(bit_position == 100 * 8 + 2);
    }

    SECTION("Block size exceeds limit - skip detection")
    {
        size_t size = WT_BITFLIP_MAX_SIZE + 1;
        std::vector<uint8_t> data(size, 0);

        // Initialize with pattern.
        for (size_t i = 0; i < size; ++i)
            data[i] = static_cast<uint8_t>(i);

        uint32_t checksum = __wt_checksum(data.data(), data.size());

        // Should return false immediately without checking.
        bool found = __ut_block_bitflip_detect(data.data(), data.size(), checksum, &bit_position);

        CHECK(found == false);
    }
}

TEST_CASE("Block bitflip detection: all bit positions", "[block_bitflip]")
{
    checksum_fixture fixture;
    // Test that we can detect a flip in each of the 8 bit positions.
    std::vector<uint8_t> data(32, 0);

    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>(i * 13 + 7);

    SECTION("Test each bit position in a single byte")
    {
        for (size_t bit = 0; bit < 8; ++bit) {
            size_t bit_position = 0;
            size_t target_byte = 15;

            // Calculate correct checksum.
            uint32_t correct_checksum = __wt_checksum(data.data(), data.size());

            // Flip the target bit.
            data[target_byte] ^= (1U << bit);

            // Detect the flip.
            bool found =
              __ut_block_bitflip_detect(data.data(), data.size(), correct_checksum, &bit_position);

            CHECK(found == true);
            CHECK(bit_position == target_byte * 8 + bit);

            // Flip back for next iteration.
            data[target_byte] ^= (1U << bit);
        }
    }
}

TEST_CASE("Block bitflip detection: edge cases", "[block_bitflip]")
{
    checksum_fixture fixture;
    size_t bit_position = 0;

    SECTION("Single byte buffer")
    {
        std::vector<uint8_t> data(1, 0x42);

        uint32_t correct_checksum = __wt_checksum(data.data(), data.size());

        // Flip bit 5.
        data[0] ^= (1U << 5);

        bool found =
          __ut_block_bitflip_detect(data.data(), data.size(), correct_checksum, &bit_position);

        CHECK(found == true);
        CHECK(bit_position == 5);
    }

    SECTION("All zeros with single bit flip")
    {
        std::vector<uint8_t> data(100, 0);

        uint32_t correct_checksum = __wt_checksum(data.data(), data.size());

        // Flip one bit.
        data[50] ^= 1;

        bool found =
          __ut_block_bitflip_detect(data.data(), data.size(), correct_checksum, &bit_position);

        CHECK(found == true);
        CHECK(bit_position == 50 * 8);
    }

    SECTION("All ones with single bit flip")
    {
        std::vector<uint8_t> data(100, 0xFF);

        uint32_t correct_checksum = __wt_checksum(data.data(), data.size());

        // Flip one bit (changing 1 to 0).
        data[33] ^= (1U << 4);

        bool found =
          __ut_block_bitflip_detect(data.data(), data.size(), correct_checksum, &bit_position);

        CHECK(found == true);
        CHECK(bit_position == 33 * 8 + 4);
    }
}

TEST_CASE("Block bitflip detection: data integrity", "[block_bitflip]")
{
    checksum_fixture fixture;
    std::vector<uint8_t> data(256, 0);
    size_t bit_position = 0;

    SECTION("Data is restored after detection")
    {
        // Initialize with a known pattern.
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<uint8_t>(i);

        // Save original data.
        std::vector<uint8_t> original_data = data;

        uint32_t correct_checksum = __wt_checksum(data.data(), data.size());

        // Flip a bit.
        data[128] ^= (1U << 3);

        // Run detection.
        bool found =
          __ut_block_bitflip_detect(data.data(), data.size(), correct_checksum, &bit_position);

        CHECK(found == true);

        // Data should be restored to the corrupted state (with the one flipped bit).
        // The function flips bits back after testing each one.
        CHECK(data[128] == (original_data[128] ^ (1U << 3)));

        // All other bytes should be unchanged.
        for (size_t i = 0; i < data.size(); ++i) {
            if (i != 128) {
                CHECK(data[i] == original_data[i]);
            }
        }
    }
}
