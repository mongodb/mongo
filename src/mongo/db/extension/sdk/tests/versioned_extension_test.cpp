// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/extension/sdk/versioned_extension.h"

#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension::sdk {
namespace {

VersionedExtension makeTestVersionedExtension(uint32_t major, uint32_t minor) {
    return VersionedExtension{::MongoExtensionAPIVersion{major, minor},
                              []() -> std::unique_ptr<sdk::Extension> {
                                  return nullptr;
                              }};
}

class VersionedExtensionGreaterComparatorTest : public unittest::Test {
protected:
    VersionedExtensionGreaterComparator comp;
};


TEST_F(VersionedExtensionGreaterComparatorTest, ComparesMajorVersionsCorrectly) {
    auto v2_0 = makeTestVersionedExtension(2, 0);
    auto v1_9 = makeTestVersionedExtension(1, 9);

    ASSERT_TRUE(comp(v2_0, v1_9));
    ASSERT_FALSE(comp(v1_9, v2_0));
}

TEST_F(VersionedExtensionGreaterComparatorTest, ComparesMinorVersionsCorrectly) {
    auto v1_2 = makeTestVersionedExtension(1, 2);
    auto v1_1 = makeTestVersionedExtension(1, 1);

    ASSERT_TRUE(comp(v1_2, v1_1));
    ASSERT_FALSE(comp(v1_1, v1_2));
}

TEST_F(VersionedExtensionGreaterComparatorTest, ComparesEqualVersionsCorrectly) {
    auto v1_2_a = makeTestVersionedExtension(1, 2);
    auto v1_2_b = makeTestVersionedExtension(1, 2);

    ASSERT_FALSE(comp(v1_2_a, v1_2_b));
    ASSERT_FALSE(comp(v1_2_b, v1_2_a));
    ASSERT_FALSE(comp(v1_2_a, v1_2_a));
}

TEST_F(VersionedExtensionGreaterComparatorTest, OrdersSetCorrectly) {
    std::set<VersionedExtension, VersionedExtensionGreaterComparator> versionedSet;

    versionedSet.insert(makeTestVersionedExtension(1, 10));
    versionedSet.insert(makeTestVersionedExtension(2, 0));
    versionedSet.insert(makeTestVersionedExtension(0, 9));
    versionedSet.insert(makeTestVersionedExtension(2, 1));
    versionedSet.insert(makeTestVersionedExtension(1, 9));

    std::vector<::MongoExtensionAPIVersion> correctlyOrderedVersions = {
        ::MongoExtensionAPIVersion{2, 1},
        ::MongoExtensionAPIVersion{2, 0},
        ::MongoExtensionAPIVersion{1, 10},
        ::MongoExtensionAPIVersion{1, 9},
        ::MongoExtensionAPIVersion{0, 9}};

    auto setIt = versionedSet.begin();

    for (const auto& version : correctlyOrderedVersions) {
        ASSERT_EQUALS(version.major, setIt->version.major);
        ASSERT_EQUALS(version.minor, setIt->version.minor);
        setIt++;
    }

    ASSERT_EQUALS(setIt, versionedSet.end());
}

TEST_F(VersionedExtensionGreaterComparatorTest, OrderedSetBlocksDoubleInsert) {
    std::set<VersionedExtension, VersionedExtensionGreaterComparator> versionedSet;

    ASSERT_TRUE(versionedSet.insert(makeTestVersionedExtension(1, 2)).second);
    ASSERT_FALSE(versionedSet.insert(makeTestVersionedExtension(1, 2)).second);
}

TEST(VersionedExtensionContainerTest, BuildVersionsListEmptyByDefault) {
    VersionedExtensionContainer container;
    ASSERT_TRUE(container.getVersionsList().empty());
}

TEST(VersionedExtensionContainerTest, BuildVersionsListReturnsRegisteredVersionsDescending) {
    VersionedExtensionContainer container;
    container.registerVersion(::MongoExtensionAPIVersion{1, 10}, []() { return nullptr; });
    container.registerVersion(::MongoExtensionAPIVersion{2, 0}, []() { return nullptr; });
    container.registerVersion(::MongoExtensionAPIVersion{0, 9}, []() { return nullptr; });
    container.registerVersion(::MongoExtensionAPIVersion{1, 9}, []() { return nullptr; });

    const std::vector<::MongoExtensionAPIVersion> expected = {::MongoExtensionAPIVersion{2, 0},
                                                              ::MongoExtensionAPIVersion{1, 10},
                                                              ::MongoExtensionAPIVersion{1, 9},
                                                              ::MongoExtensionAPIVersion{0, 9}};

    const auto versions = container.getVersionsList();
    ASSERT_EQUALS(versions.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQUALS(versions[i].major, expected[i].major);
        ASSERT_EQUALS(versions[i].minor, expected[i].minor);
    }
}

}  // namespace
}  // namespace mongo::extension::sdk
