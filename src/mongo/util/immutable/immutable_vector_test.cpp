// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/unittest/unittest.h"
#include "mongo/util/immutable/vector.h"

#include <type_traits>
#include <utility>

#include <immer/detail/iterator_facade.hpp>
#include <immer/vector.hpp>
#include <immer/vector_transient.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class UserDefinedType {
public:
    UserDefinedType() = default;
    explicit UserDefinedType(int val) : a(val) {}

    bool operator==(const UserDefinedType& rhs) const {
        return a == rhs.a;
    }

private:
    int a = 0;
};

TEST(ImmutableUnorderedMap, Basic) {
    // Insert some values and verify that the data structure is behaving as expected
    immutable::vector<int> v0;
    auto v1 = v0.push_back(1);
    // Record the pointer to the value at index '0', verify that this doesn't change after
    // performing  additional modifications
    auto* v1Val = &(*v1.begin());

    // Create distinct branches of the history from v1. v0 and v1 should be unaffected
    auto v2 = v1.update(0, [](int v) { return v += 1; });
    auto v3 = v1.push_back(3);
    auto v4 = v1.set(0, 4);

    // Verify that values are as expected
    ASSERT_EQ(v0.size(), 0);

    ASSERT_EQ(v1.size(), 1);
    ASSERT_EQ(v1.at(0), 1);

    ASSERT_EQ(v2.size(), 1);
    ASSERT_EQ(v2.at(0), 2);

    ASSERT_EQ(v3.size(), 2);
    ASSERT_EQ(v3.at(0), 1);
    ASSERT_EQ(v3.at(1), 3);

    ASSERT_EQ(v4.size(), 1);
    ASSERT_EQ(v4.at(0), 4);

    // Verify that pointer to v1's value did not change
    ASSERT_EQ(&(*v1.begin()), v1Val);
}

TEST(ImmutableUnorderedMap, UserDefinedType) {
    immutable::vector<UserDefinedType> v0;
    auto v1 = v0.push_back(UserDefinedType(1));
    ASSERT_EQ(v1.at(0), UserDefinedType(1));
}

TEST(ImmutableUnorderedMap, BatchWrite) {
    immutable::vector<int> v0;

    auto transient = v0.transient();
    transient.push_back(1);
    transient.push_back(2);
    immutable::vector<int> v1 = transient.persistent();

    ASSERT_EQ(v0.size(), 0);
    ASSERT_EQ(v1.size(), 2);
    ASSERT_EQ(v1.at(0), 1);
    ASSERT_EQ(v1.at(1), 2);
}

TEST(ImmutableUnorderedMap, ExclusiveOwnership) {
    immutable::vector<int> v0;

    auto v1 = v0.push_back(1);
    auto v2 = v1.push_back(2);
    auto v3 = v2.push_back(3);

    ASSERT_EQ(v1.size(), 1);
    ASSERT_EQ(v2.size(), 2);
    ASSERT_EQ(v3.size(), 3);

    // Claiming exclusive ownership over v3 means v3 will no longer be valid after mutation, but
    // older versions should be unperturbed.
    auto v4 = std::move(v3).take(0);
    ASSERT_EQ(v1.size(), 1);
    ASSERT_EQ(v2.size(), 2);
    ASSERT_EQ(v4.size(), 0);
}

}  // namespace
}  // namespace mongo
