/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/extension/sdk/api_version_vector_to_span.h"

#include "mongo/db/extension/public/api.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

#include <cstdint>
#include <memory>

namespace mongo::extension::sdk {
namespace {

/**
 * Test fixture for to_span tests with MongoExtensionAPIVersionVector.
 */
class ApiVersionVectorToSpan : public unittest::Test {
protected:
    void setUp() override {
        // Allocate array of versions.
        _versions = std::make_unique<::MongoExtensionAPIVersion[]>(4);
        _versions[0] = {1, 0};
        _versions[1] = {1, 1};
        _versions[2] = {2, 0};
        _versions[3] = {2, 1};
    }

    void tearDown() override {
        _versions.reset();
    }

    ::MongoExtensionAPIVersionVector makeVector(uint64_t count) {
        return ::MongoExtensionAPIVersionVector{count, _versions.get()};
    }

    std::unique_ptr<::MongoExtensionAPIVersion[]> _versions;
};

/**
 * Test fixture for edge cases.
 */
class ApiVersionVectorToSpanEdgeCaseTest : public unittest::Test {
protected:
    ::MongoExtensionAPIVersionVector emptyVector{0, nullptr};
    ::MongoExtensionAPIVersion singleVersion{42, 99};
    ::MongoExtensionAPIVersionVector singleElementVector{1, &singleVersion};
};

// Edge case tests.
TEST_F(ApiVersionVectorToSpanEdgeCaseTest, EmptyVector) {
    auto span = to_span(&emptyVector);

    ASSERT_EQ(span.size(), 0u);
    ASSERT_TRUE(span.empty());
}

TEST_F(ApiVersionVectorToSpanEdgeCaseTest, SingleElement) {
    auto span = to_span(&singleElementVector);

    ASSERT_EQ(span.size(), 1u);
    ASSERT_FALSE(span.empty());
    ASSERT_EQ(span[0].major, 42u);
    ASSERT_EQ(span[0].minor, 99u);
}

// Basic functionality tests.
TEST_F(ApiVersionVectorToSpan, MultipleElements) {
    auto vec = makeVector(4);
    auto span = to_span(&vec);

    ASSERT_EQ(span.size(), 4u);
    ASSERT_EQ(span[0].major, 1u);
    ASSERT_EQ(span[0].minor, 0u);
    ASSERT_EQ(span[1].major, 1u);
    ASSERT_EQ(span[1].minor, 1u);
    ASSERT_EQ(span[2].major, 2u);
    ASSERT_EQ(span[2].minor, 0u);
    ASSERT_EQ(span[3].major, 2u);
    ASSERT_EQ(span[3].minor, 1u);
}

TEST_F(ApiVersionVectorToSpan, RangeBasedForLoop) {
    auto vec = makeVector(3);
    auto span = to_span(&vec);

    size_t count = 0;
    for (const auto& version : span) {
        ASSERT_EQ(version.major, _versions[count].major);
        ASSERT_EQ(version.minor, _versions[count].minor);
        ++count;
    }
    ASSERT_EQ(count, 3u);
}

TEST_F(ApiVersionVectorToSpan, IndexAccess) {
    auto vec = makeVector(4);
    auto span = to_span(&vec);

    ASSERT_EQ(span[0].major, 1u);
    ASSERT_EQ(span[0].minor, 0u);
    ASSERT_EQ(span[3].major, 2u);
    ASSERT_EQ(span[3].minor, 1u);
}

// Type safety tests.
TEST_F(ApiVersionVectorToSpan, ConstCorrectness) {
    auto vec = makeVector(1);
    auto span = to_span(&vec);

    // Verify span is const.
    static_assert(std::is_const_v<std::remove_reference_t<decltype(span[0])>>);
}

TEST_F(ApiVersionVectorToSpan, TypeDeduction) {
    auto vec = makeVector(1);
    auto span = to_span(&vec);

    using SpanType = decltype(span);
    using ElementType = typename SpanType::element_type;

    static_assert(std::is_same_v<ElementType, const MongoExtensionAPIVersion>);
}

TEST_F(ApiVersionVectorToSpan, IteratorCompatibility) {
    auto vec = makeVector(3);
    auto span = to_span(&vec);

    auto it = span.begin();
    ASSERT_EQ(it->major, 1u);
    ASSERT_EQ(it->minor, 0u);

    ++it;
    ASSERT_EQ(it->major, 1u);
    ASSERT_EQ(it->minor, 1u);

    ++it;
    ASSERT_EQ(it->major, 2u);
    ASSERT_EQ(it->minor, 0u);

    ++it;
    ASSERT_EQ(it, span.end());
}

}  // namespace
}  // namespace mongo::extension::sdk
