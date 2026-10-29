// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
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
