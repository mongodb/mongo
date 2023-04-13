/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/unittest/unittest.h"

#include "mongo/util/immutable/vector.h"

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
