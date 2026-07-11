// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/unittest/unittest.h"
#include "mongo/util/immutable/unordered_map.h"
#include "mongo/util/immutable/unordered_set.h"
#include "mongo/util/string_map.h"

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <immer/detail/util.hpp>
#include <immer/map.hpp>
#include <immer/map_transient.hpp>
#include <immer/set.hpp>
#include <immer/set_transient.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

class UserDefinedKey {
public:
    UserDefinedKey() = default;
    explicit UserDefinedKey(int val) : a(val) {}

    bool operator==(const UserDefinedKey& rhs) const {
        return a == rhs.a;
    }

    // Use the Abseil hashing framework
    template <typename H>
    friend H AbslHashValue(H h, const UserDefinedKey& obj) {
        return H::combine(std::move(h), obj.a);
    }

private:
    int a = 0;
};

TEST(ImmutableUnorderedMap, Basic) {
    // Insert some values and verify that the data structure is behaving as expected
    immutable::unordered_map<int, int> v0;
    auto v1 = v0.set(1, 2);
    // Record the pointer to the value '1', verify that this doesn't change after performing more
    // inserts
    auto v1Val = v1.find(1);

    // Create distinct branches of the history from v1. v0 and v1 should be unaffected
    auto v2 = v1.update(1, [](int v) { return v += 1; });
    auto v3 = v1.set(2, 3);

    // Verify that values are as expected
    ASSERT_EQ(v0.size(), 0);

    ASSERT_EQ(v1.size(), 1);
    ASSERT(v1.find(1));
    ASSERT_EQ(*v1.find(1), 2);

    ASSERT_EQ(v2.size(), 1);
    ASSERT(v2.find(1));
    ASSERT_EQ(*v2.find(1), 3);
    ASSERT(!v2.find(2));

    ASSERT_EQ(v3.size(), 2);
    ASSERT(v3.find(1));
    ASSERT_EQ(*v3.find(1), 2);
    ASSERT(v3.find(2));
    ASSERT_EQ(*v3.find(2), 3);

    // Verify that pointer to v1's value did not change
    ASSERT_EQ(v1.find(1), v1Val);
}

TEST(ImmutableUnorderedMap, UserDefinedType) {
    immutable::unordered_map<UserDefinedKey, int> v0;
    auto v1 = v0.set(UserDefinedKey(1), 2);
    ASSERT(v1.find(UserDefinedKey(1)));
}

TEST(ImmutableUnorderedMap, HeterogeneousLookup) {
    immutable::unordered_map<std::string, int, StringMapHasher, StringMapEq> v0;
    auto v1 = v0.set("str", 1);

    // Lookup using std::string_view without the need to convert to string.
    ASSERT(v1.find("str"sv));

    // Lookup using pre-hash
    StringMapHashedKey hashedKey = StringMapHasher().hashed_key("str"sv);
    ASSERT(v1.find(hashedKey));
}

TEST(ImmutableUnorderedMap, BatchWrite) {
    immutable::unordered_map<int, int> v0;

    auto transient = v0.transient();
    transient.set(1, 2);
    transient.set(2, 3);
    immutable::unordered_map<int, int> v1 = transient.persistent();

    ASSERT(!v0.find(1));
    ASSERT(!v0.find(2));
    ASSERT(v1.find(1));
    ASSERT(v1.find(2));
}

TEST(ImmutableUnorderedMap, ExclusiveOwnership) {
    immutable::unordered_map<int, int> v0;

    auto v1 = v0.set(1, 1);
    auto v2 = v1.set(2, 2);
    auto v3 = v2.set(3, 3);

    ASSERT(v1.find(1));
    ASSERT(v2.find(1));
    ASSERT(v3.find(1));

    // Claiming exclusive ownership over v3 means v3 will no longer be valid after mutation, but
    // older versions should be unperturbed.
    auto v4 = std::move(v3).erase(1);
    ASSERT(v1.find(1));
    ASSERT(v2.find(1));
    ASSERT(!v4.find(1));
}

TEST(ImmutableUnorderedSet, Basic) {
    // Insert some values and verify that the data structure is behaving as expected
    immutable::unordered_set<int> v0;
    auto v1 = v0.insert(1);
    // Record the pointer to the value '1', verify that this doesn't change after performing more
    // inserts
    auto v1Val = v1.find(1);

    // Make more versions of the data structure, v2 and v3 are now distinct history branches from
    // v1. v0 and v1 should be unaffected
    auto v2 = v1.insert(2);
    auto v3 = v1.insert(2);

    // Verify that values are as expected
    ASSERT_EQ(v0.size(), 0);

    ASSERT_EQ(v1.size(), 1);
    ASSERT(v1.find(1));
    ASSERT_EQ(*v1.find(1), 1);

    ASSERT_EQ(v2.size(), 2);
    ASSERT(v2.find(1));
    ASSERT_EQ(*v2.find(1), 1);
    ASSERT(v2.find(2));
    ASSERT_EQ(*v2.find(2), 2);

    ASSERT_EQ(v3.size(), 2);
    ASSERT(v3.find(1));
    ASSERT_EQ(*v3.find(1), 1);
    ASSERT(v3.find(2));
    ASSERT_EQ(*v3.find(2), 2);

    // Verify that pointer to v1's value did not change
    ASSERT_EQ(v1.find(1), v1Val);
    // Key is stored in v2 and v3 with different addresses
    ASSERT_NE(v2.find(2), v3.find(2));
}

TEST(ImmutableUnorderedSet, UserDefinedType) {
    immutable::unordered_set<UserDefinedKey> v0;
    auto v1 = v0.insert(UserDefinedKey(1));
    ASSERT(v1.find(UserDefinedKey(1)));
}

TEST(ImmutableUnorderedSet, HeterogeneousLookup) {
    immutable::unordered_set<std::string, StringMapHasher, StringMapEq> v0;
    auto v1 = v0.insert("str");

    // Lookup using std::string_view without the need to convert to string.
    ASSERT(v1.find("str"sv));

    // Lookup using pre-hash
    StringMapHashedKey hashedKey = StringMapHasher().hashed_key("str"sv);
    ASSERT(v1.find(hashedKey));
}

TEST(ImmutableUnorderedSet, BatchWrite) {
    immutable::unordered_set<int> v0;

    auto transient = v0.transient();
    transient.insert(1);
    transient.insert(2);
    immutable::unordered_set<int> v1 = transient.persistent();

    ASSERT(!v0.find(1));
    ASSERT(!v0.find(2));
    ASSERT(v1.find(1));
    ASSERT(v1.find(2));
}

TEST(ImmutableUnorderedSet, ExclusiveOwnership) {
    immutable::unordered_set<int> v0;

    auto v1 = v0.insert(1);
    auto v2 = v1.insert(2);
    auto v3 = v2.insert(3);

    ASSERT(v1.find(1));
    ASSERT(v2.find(1));
    ASSERT(v3.find(1));

    auto v4 = std::move(v3).erase(1);
    ASSERT(v1.find(1));
    ASSERT(v2.find(1));
    ASSERT(!v4.find(1));
}

}  // namespace
}  // namespace mongo
