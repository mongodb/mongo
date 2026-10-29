// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/fts/unicode/byte_vector.h"

#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <cstring>
#include <iterator>
#include <numeric>

#ifdef MONGO_HAVE_FAST_BYTE_VECTOR
namespace mongo {
namespace unicode {

TEST(ByteVector, LoadStoreUnaligned) {
    uint8_t inputBuf[ByteVector::size * 2];
    uint8_t outputBuf[ByteVector::size * 2];
    std::iota(std::begin(inputBuf), std::end(inputBuf), 0);

    // Try loads and stores at all possible (mis)alignments.
    for (size_t offset = 0; offset < ByteVector::size; offset++) {
        std::memset(outputBuf, 0, sizeof(outputBuf));
        ByteVector::load(inputBuf + offset).store(outputBuf + offset);

        for (size_t i = 0; i < ByteVector::size; i++) {
            ASSERT_EQ(outputBuf[offset + i], inputBuf[offset + i]);
        }
    }
}

TEST(ByteVector, Splat) {
    uint8_t outputBuf[ByteVector::size] = {};
    ByteVector(0x12).store(outputBuf);

    for (size_t i = 0; i < ByteVector::size; i++) {
        ASSERT_EQ(outputBuf[i], 0x12);
    }
}

TEST(ByteVector, MaskAny) {
    uint8_t inputBuf[ByteVector::size];
    std::memset(inputBuf, 0xFF, sizeof(inputBuf));
    for (size_t offset = 0; offset <= ByteVector::size; offset++) {
        auto mask = ByteVector::load(inputBuf).maskAny();
        ASSERT_EQ(ByteVector::countInitialZeros(mask), offset);
        if (offset < ByteVector::size) {
            inputBuf[offset] = 0;  // Add an initial 0 for the next loop.
        }
    }
}

TEST(ByteVector, MaskHigh) {
    uint8_t inputBuf[ByteVector::size];
    std::memset(inputBuf, 0x80, sizeof(inputBuf));
    for (size_t offset = 0; offset <= ByteVector::size; offset++) {
        auto mask = ByteVector::load(inputBuf).maskHigh();
        ASSERT_EQ(ByteVector::countInitialZeros(mask), offset);
        if (offset < ByteVector::size) {
            inputBuf[offset] = 0x7f;  // Add an initial 0 bit for the next loop.
        }
    }
}

TEST(ByteVector, CompareEQ) {
    uint8_t inputBuf[ByteVector::size];
    uint8_t outputBuf[ByteVector::size] = {};
    std::iota(std::begin(inputBuf), std::end(inputBuf), 0);

    ByteVector::load(inputBuf).compareEQ(3).store(outputBuf);

    for (size_t i = 0; i < ByteVector::size; i++) {
        ASSERT_EQ(outputBuf[i], inputBuf[i] == 3 ? 0xFF : 0x00);
    }
}

TEST(ByteVector, CompareGT) {
    uint8_t inputBuf[ByteVector::size];
    uint8_t outputBuf[ByteVector::size] = {};
    std::iota(std::begin(inputBuf), std::end(inputBuf), 0);

    ByteVector::load(inputBuf).compareGT(3).store(outputBuf);

    for (size_t i = 0; i < ByteVector::size; i++) {
        ASSERT_EQ(outputBuf[i], inputBuf[i] > 3 ? 0xFF : 0x00);
    }
}

TEST(ByteVector, CompareLT) {
    uint8_t inputBuf[ByteVector::size];
    uint8_t outputBuf[ByteVector::size] = {};
    std::iota(std::begin(inputBuf), std::end(inputBuf), 0);

    ByteVector::load(inputBuf).compareLT(3).store(outputBuf);

    for (size_t i = 0; i < ByteVector::size; i++) {
        ASSERT_EQ(outputBuf[i], inputBuf[i] < 3 ? 0xFF : 0x00);
    }
}

TEST(ByteVector, BitOr) {
    uint8_t inputBuf[ByteVector::size];
    uint8_t outputBuf[ByteVector::size] = {};
    std::iota(std::begin(inputBuf), std::end(inputBuf), 0);

    (ByteVector::load(inputBuf) | ByteVector(0x1)).store(outputBuf);

    for (size_t i = 0; i < ByteVector::size; i++) {
        ASSERT_EQ(outputBuf[i], inputBuf[i] | 1);
    }
}

TEST(ByteVector, BitOrAssign) {
    uint8_t inputBuf[ByteVector::size];
    uint8_t outputBuf[ByteVector::size] = {};
    std::iota(std::begin(inputBuf), std::end(inputBuf), 0);

    auto vec = ByteVector::load(inputBuf);
    vec |= ByteVector(2);
    vec.store(outputBuf);

    for (size_t i = 0; i < ByteVector::size; i++) {
        ASSERT_EQ(outputBuf[i], inputBuf[i] | 2);
    }
}

TEST(ByteVector, BitAnd) {
    uint8_t inputBuf[ByteVector::size];
    uint8_t outputBuf[ByteVector::size] = {};
    std::iota(std::begin(inputBuf), std::end(inputBuf), 0);

    (ByteVector::load(inputBuf) & ByteVector(2)).store(outputBuf);

    for (size_t i = 0; i < ByteVector::size; i++) {
        ASSERT_EQ(outputBuf[i], inputBuf[i] & 2);
    }
}

TEST(ByteVector, BitAndAssign) {
    uint8_t inputBuf[ByteVector::size];
    uint8_t outputBuf[ByteVector::size] = {};
    std::iota(std::begin(inputBuf), std::end(inputBuf), 0);

    auto vec = ByteVector::load(inputBuf);
    vec &= ByteVector(2);
    vec.store(outputBuf);

    for (size_t i = 0; i < ByteVector::size; i++) {
        ASSERT_EQ(outputBuf[i], inputBuf[i] & 2);
    }
}

}  // namespace unicode
}  // namespace mongo
#else
// Our unittest framework gets angry if there are no tests. If we don't have ByteVector, give it a
// dummy test to make it happy.
TEST(ByteVector, ByteVectorNotSupportedOnThisPlatform) {}
#endif
