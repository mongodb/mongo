// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <array>
#include <cstddef>
#include <cstdint>

#include <wiredtiger.h>

#include <gtest/gtest.h>

namespace {

uint32_t referenceChecksum(uint32_t seed, const uint8_t* data, size_t size) {
    uint32_t crc = ~seed;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78U : 0);
        }
    }
    return ~crc;
}

TEST(WiredTigerChecksumTest, MatchesCrc32cVector) {
    constexpr char data[] = "123456789";
    EXPECT_EQ(wiredtiger_crc32c_func()(data, sizeof(data) - 1), 0xe3069283U);
}

TEST(WiredTigerChecksumTest, HandlesSeedsAlignmentAndTrailingBytes) {
    alignas(8) std::array<uint8_t, 72> data{};
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i * 37);
    }
    const auto checksum = wiredtiger_crc32c_func();
    const auto seededChecksum = wiredtiger_crc32c_with_seed_func();
    for (size_t offset = 0; offset < 8; ++offset) {
        for (size_t size = 0; size <= 64; ++size) {
            SCOPED_TRACE(testing::Message() << "offset=" << offset << " size=" << size);
            const auto* bytes = data.data() + offset;
            EXPECT_EQ(checksum(bytes, size), referenceChecksum(0, bytes, size));
            for (uint32_t seed : {0U, 1U, 0x12345678U, 0xffffffffU}) {
                EXPECT_EQ(seededChecksum(seed, bytes, size), referenceChecksum(seed, bytes, size));
                const size_t split = size / 2;
                EXPECT_EQ(
                    seededChecksum(seededChecksum(seed, bytes, split), bytes + split, size - split),
                    referenceChecksum(seed, bytes, size));
            }
        }
    }
}

}  // namespace
