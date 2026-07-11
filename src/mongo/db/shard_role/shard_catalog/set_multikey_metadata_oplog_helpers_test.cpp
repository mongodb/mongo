// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/set_multikey_metadata_oplog_helpers.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <string_view>

namespace mongo {
namespace {

constexpr auto kVersion = key_string::Version::kLatestVersion;
constexpr auto kRsKeyFormat = KeyFormat::Long;

KeyStringSet generateMultikeyMetadataIndexKeys(const BSONObj& keyPattern,
                                               const BSONObj& doc,
                                               KeyFormat rsKeyFormat = kRsKeyFormat) {
    const auto ordering = WildcardAccessMethod::makeOrdering(keyPattern);
    WildcardKeyGenerator gen(keyPattern, BSONObj(), nullptr, kVersion, ordering, rsKeyFormat);
    SharedBufferFragmentBuilder buf(key_string::HeapBuilder::kHeapAllocatorDefaultBytes);
    KeyStringSet indexKeys;
    KeyStringSet metadataKeys;
    gen.generateKeys(buf, doc, &indexKeys, &metadataKeys);
    return metadataKeys;
}

void assertKeyStringSetsEqual(const KeyStringSet& a,
                              const KeyStringSet& b,
                              std::string_view context) {
    ASSERT_EQ(a.size(), b.size()) << context;
    auto itA = a.begin();
    auto itB = b.begin();
    for (; itA != a.end(); ++itA, ++itB) {
        ASSERT_EQ(0, itA->compare(*itB)) << "KeyString mismatch in: " << context;
    }
}

void verifyRoundTrip(const BSONObj& keyPattern,
                     const KeyStringSet& originalKeys,
                     KeyFormat rsKeyFormat,
                     std::string_view context) {
    const auto ordering = WildcardAccessMethod::makeOrdering(keyPattern);

    auto fieldPaths = set_multikey_metadata_oplog_helpers::extractFieldPathsFromMetadataKeys(
        originalKeys, ordering);
    auto pathsObj = set_multikey_metadata_oplog_helpers::fieldPathsToBSON(fieldPaths);
    auto regeneratedKeys =
        set_multikey_metadata_oplog_helpers::regenerateMetadataKeysFromFieldPaths(
            pathsObj, kVersion, ordering, rsKeyFormat, keyPattern);

    assertKeyStringSetsEqual(originalKeys, regeneratedKeys, context);
}

TEST(SetMultikeyMetadataOplogHelpersTest, RoundTripSimple) {
    const auto keyPattern = BSON("$**" << 1);

    KeyStringSet originalKeys;
    for (const auto& doc : {BSON("a" << BSON_ARRAY(1 << 2)),
                            BSON("b" << BSON("c" << BSON_ARRAY(1 << 2))),
                            BSON("d" << BSON("e" << BSON("f" << BSON_ARRAY(1))))}) {
        auto docKeys = generateMultikeyMetadataIndexKeys(keyPattern, doc);
        originalKeys.insert(docKeys.begin(), docKeys.end());
    }
    ASSERT_EQ(3u, originalKeys.size());

    verifyRoundTrip(keyPattern, originalKeys, kRsKeyFormat, "simple wildcard");
}

TEST(SetMultikeyMetadataOplogHelpersTest, RoundTripCompoundWildcard) {
    const auto keyPattern = BSON("x" << 1 << "$**" << 1 << "y" << 1);

    KeyStringSet originalKeys;
    for (const auto& doc : {BSON("x" << 10 << "a" << BSON_ARRAY(1 << 2) << "y" << 20),
                            BSON("x" << 30 << "b" << BSON("c" << BSON_ARRAY(3)) << "y" << 40)}) {
        auto docKeys = generateMultikeyMetadataIndexKeys(keyPattern, doc);
        originalKeys.insert(docKeys.begin(), docKeys.end());
    }
    ASSERT_EQ(2u, originalKeys.size());

    verifyRoundTrip(keyPattern, originalKeys, kRsKeyFormat, "compound wildcard");
}

TEST(SetMultikeyMetadataOplogHelpersTest, RoundTripDescendingCompoundWildcard) {
    const auto keyPattern = BSON("x" << -1 << "$**" << 1 << "y" << -1);

    auto originalKeys = generateMultikeyMetadataIndexKeys(
        keyPattern, BSON("x" << 1 << "path" << BSON_ARRAY(1 << 2) << "y" << 2));
    ASSERT_EQ(1u, originalKeys.size());

    verifyRoundTrip(keyPattern, originalKeys, kRsKeyFormat, "descending compound wildcard");
}

TEST(SetMultikeyMetadataOplogHelpersTest, RoundTripEmpty) {
    const auto keyPattern = BSON("$**" << 1);

    KeyStringSet emptyKeys;
    verifyRoundTrip(keyPattern, emptyKeys, kRsKeyFormat, "empty");
}

TEST(SetMultikeyMetadataOplogHelpersTest, RoundTripStringKeyFormat) {
    const auto keyPattern = BSON("$**" << 1);
    auto originalKeys = generateMultikeyMetadataIndexKeys(
        keyPattern, BSON("field" << BSON_ARRAY(1 << 2)), KeyFormat::String);
    ASSERT_EQ(1u, originalKeys.size());

    verifyRoundTrip(keyPattern, originalKeys, KeyFormat::String, "string key format");
}

TEST(SetMultikeyMetadataOplogHelpersTest, FieldPathsToBSONFormat) {
    auto pathsObj = set_multikey_metadata_oplog_helpers::fieldPathsToBSON({"a", "b.c"});
    ASSERT_EQ(BSON_ARRAY("a" << "b.c").woCompare(pathsObj), 0);
}

}  // namespace
}  // namespace mongo
