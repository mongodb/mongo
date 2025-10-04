/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/util/container_size_helper.h"

#include "mongo/base/string_data.h"
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
