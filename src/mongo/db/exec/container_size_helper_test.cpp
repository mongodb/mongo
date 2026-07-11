// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/container_size_helper.h"

#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

struct Mock {
    int size;
};

TEST(ContainerSizeHelper, TestEstimateObjectSizeInBytes) {
    std::vector<Mock> vect = {{1}, {2}, {3}, {4}};

    // Sum of 'size' of each element '1 + 2 + 3 + 4'.
    uint64_t expectedSize = 10;

    // When 'includeComplieTimeSize' is false should return only the sum of sizes calculated by the
    // 'function'.
    ASSERT_EQ(mongo::container_size_helper::estimateObjectSizeInBytes(
                  vect, [](const auto& obj) { return obj.size; }, false),
              expectedSize);

    // When 'includeShallowSize' is true, should add size of 'Mock' object.
    ASSERT_EQ(mongo::container_size_helper::estimateObjectSizeInBytes(
                  vect, [](const auto& obj) { return obj.size; }, true),
              expectedSize + sizeof(Mock) * vect.capacity());
}

TEST(ContainerSizeHelper, TestEstimateObjectSizeInBytesWithPointers) {
    Mock obj1 = {2};
    Mock obj2 = {1};
    std::vector<Mock*> vect = {&obj1, &obj1, &obj2};

    // Sum of 'size' of each element '2 + 2 + 1'.
    uint64_t expectedSize = 5;

    // Reserve extra space for the vector.
    vect.reserve(10);
    ASSERT_EQ(static_cast<size_t>(10), vect.capacity());

    // When 'includeShallowSize' is true, should add size of 'Mock*' pointer.
    ASSERT_EQ(mongo::container_size_helper::estimateObjectSizeInBytes(
                  vect, [](const auto& obj) { return obj->size; }, true),
              expectedSize + sizeof(Mock*) * vect.capacity());
}
}  // namespace
