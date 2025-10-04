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
#include "mongo/db/extension/sdk/versioned_extension.h"

#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension::sdk {
namespace {

VersionedExtension makeTestVersionedExtension(uint32_t major, uint32_t minor, uint32_t patch) {
    return VersionedExtension{::MongoExtensionAPIVersion{major, minor, patch},
                              []() -> std::unique_ptr<sdk::Extension> {
                                  return nullptr;
                              }};
}

class VersionedExtensionGreaterComparatorTest : public unittest::Test {
protected:
    VersionedExtensionGreaterComparator comp;
};


TEST_F(VersionedExtensionGreaterComparatorTest, ComparesMajorVersionsCorrectly) {
    auto v2_0_0 = makeTestVersionedExtension(2, 0, 0);
    auto v1_9_9 = makeTestVersionedExtension(1, 9, 9);

    ASSERT_TRUE(comp(v2_0_0, v1_9_9));
    ASSERT_FALSE(comp(v1_9_9, v2_0_0));
}

TEST_F(VersionedExtensionGreaterComparatorTest, ComparesMinorVersionsCorrectly) {
    auto v1_2_0 = makeTestVersionedExtension(1, 2, 0);
    auto v1_1_9 = makeTestVersionedExtension(1, 1, 9);

    ASSERT_TRUE(comp(v1_2_0, v1_1_9));
    ASSERT_FALSE(comp(v1_1_9, v1_2_0));
}

TEST_F(VersionedExtensionGreaterComparatorTest, ComparesPatchVersionsCorrectly) {
    auto v1_0_1 = makeTestVersionedExtension(1, 0, 1);
    auto v1_0_0 = makeTestVersionedExtension(1, 0, 0);

    ASSERT_TRUE(comp(v1_0_1, v1_0_0));
    ASSERT_FALSE(comp(v1_0_0, v1_0_1));
}

TEST_F(VersionedExtensionGreaterComparatorTest, ComparesEqualVersionsCorrectly) {
    auto v1_2_3_a = makeTestVersionedExtension(1, 2, 3);
    auto v1_2_3_b = makeTestVersionedExtension(1, 2, 3);

    ASSERT_FALSE(comp(v1_2_3_a, v1_2_3_b));
    ASSERT_FALSE(comp(v1_2_3_b, v1_2_3_a));
    ASSERT_FALSE(comp(v1_2_3_a, v1_2_3_a));
}

TEST_F(VersionedExtensionGreaterComparatorTest, OrdersSetCorrectly) {
    std::set<VersionedExtension, VersionedExtensionGreaterComparator> versionedSet;

    versionedSet.insert(makeTestVersionedExtension(1, 10, 0));
    versionedSet.insert(makeTestVersionedExtension(2, 0, 1));
    versionedSet.insert(makeTestVersionedExtension(0, 9, 9));
    versionedSet.insert(makeTestVersionedExtension(2, 1, 0));
    versionedSet.insert(makeTestVersionedExtension(1, 9, 5));

    std::vector<::MongoExtensionAPIVersion> correctlyOrderedVersions = {
        ::MongoExtensionAPIVersion{2, 1, 0},
        ::MongoExtensionAPIVersion{2, 0, 1},
        ::MongoExtensionAPIVersion{1, 10, 0},
        ::MongoExtensionAPIVersion{1, 9, 5},
        ::MongoExtensionAPIVersion{0, 9, 9}};

    auto setIt = versionedSet.begin();

    for (const auto& version : correctlyOrderedVersions) {
        ASSERT_EQUALS(version.major, setIt->version.major);
        ASSERT_EQUALS(version.minor, setIt->version.minor);
        ASSERT_EQUALS(version.patch, setIt->version.patch);
        setIt++;
    }

    ASSERT_EQUALS(setIt, versionedSet.end());
}

TEST_F(VersionedExtensionGreaterComparatorTest, OrderedSetBlocksDoubleInsert) {
    std::set<VersionedExtension, VersionedExtensionGreaterComparator> versionedSet;

    ASSERT_TRUE(versionedSet.insert(makeTestVersionedExtension(1, 2, 3)).second);
    ASSERT_FALSE(versionedSet.insert(makeTestVersionedExtension(1, 2, 3)).second);
}

}  // namespace
}  // namespace mongo::extension::sdk
