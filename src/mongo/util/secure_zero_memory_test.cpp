// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/secure_zero_memory.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace mongo {

TEST(SecureZeroMemoryTest, zeroZeroLengthNull) {
    void* ptr = nullptr;
    secureZeroMemory(ptr, 0);
    ASSERT_TRUE(true);
}

DEATH_TEST(SecureZeroMemoryTestDeathTest, zeroNonzeroLengthNull, "Fatal assertion") {
    void* ptr = nullptr;
    secureZeroMemory(ptr, 1000);
}

TEST(SecureZeroMemoryTest, dataZeroed) {
    static const size_t dataSize = 100;
    std::uint8_t data[dataSize];

    // Populate array
    for (size_t i = 0; i < dataSize; ++i) {
        data[i] = i;
    }

    // Zero array
    secureZeroMemory(data, dataSize);

    // Check contents
    for (size_t i = 0; i < dataSize; ++i) {
        ASSERT_FALSE(data[i]);
    }
}

}  // namespace mongo
