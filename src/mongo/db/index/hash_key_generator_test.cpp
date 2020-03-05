/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/index/expression_keys_private.h"

#include <algorithm>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/hasher.h"
#include "mongo/db/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

const HashSeed kHashSeed = 0;
const int kHashVersion = 0;

std::string dumpKeyset(const KeyStringSet& keyStrings) {
    std::stringstream ss;
    ss << "[ ";
    for (auto& keyString : keyStrings) {
        auto key = KeyString::toBson(keyString, Ordering::make(BSONObj()));
        ss << key.toString() << " ";
    }
    ss << "]";

    return ss.str();
}

bool assertKeysetsEqual(const KeyStringSet& expectedKeys, const KeyStringSet& actualKeys) {
    if (expectedKeys.size() != actualKeys.size()) {
        LOGV2(20678,
              "Expected: {dumpKeyset_expectedKeys}, Actual: {dumpKeyset_actualKeys}",
              "dumpKeyset_expectedKeys"_attr = dumpKeyset(expectedKeys),
              "dumpKeyset_actualKeys"_attr = dumpKeyset(actualKeys));
        return false;
    }

    if (!std::equal(expectedKeys.begin(), expectedKeys.end(), actualKeys.begin())) {
        LOGV2(20679,
              "Expected: {dumpKeyset_expectedKeys}, Actual: {dumpKeyset_actualKeys}",
              "dumpKeyset_expectedKeys"_attr = dumpKeyset(expectedKeys),
              "dumpKeyset_actualKeys"_attr = dumpKeyset(actualKeys));
        return false;
    }

    return true;
}

KeyString::Value makeHashKey(BSONElement elt) {
    KeyString::HeapBuilder keyString(KeyString::Version::kLatestVersion,
                                     BSON("" << BSONElementHasher::hash64(elt, kHashSeed)),
                                     Ordering::make(BSONObj()));
    return keyString.release();
}

TEST(HashKeyGeneratorTest, CollationAppliedBeforeHashing) {
    BSONObj obj = fromjson("{a: 'string'}");
    KeyStringSet actualKeys;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj indexSpec = fromjson("{a: 'hashed'}");
    ExpressionKeysPrivate::getHashKeys(obj,
                                       indexSpec,
                                       kHashSeed,
                                       kHashVersion,
                                       false,
                                       &collator,
                                       &actualKeys,
                                       KeyString::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       false);

    BSONObj backwardsObj = fromjson("{a: 'gnirts'}");
    KeyStringSet expectedKeys;
    expectedKeys.insert(makeHashKey(backwardsObj["a"]));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(HashKeyGeneratorTest, CollationDoesNotAffectNonStringFields) {
    BSONObj obj = fromjson("{a: 5}");
    KeyStringSet actualKeys;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);

    BSONObj indexSpec = fromjson("{a: 'hashed'}");
    ExpressionKeysPrivate::getHashKeys(obj,
                                       indexSpec,
                                       kHashSeed,
                                       kHashVersion,
                                       false,
                                       &collator,
                                       &actualKeys,
                                       KeyString::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       false);

    KeyStringSet expectedKeys;
    expectedKeys.insert(makeHashKey(obj["a"]));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(HashKeyGeneratorTest, CollatorAppliedBeforeHashingNestedObject) {
    BSONObj obj = fromjson("{a: {b: 'string'}}");
    BSONObj backwardsObj = fromjson("{a: {b: 'gnirts'}}");
    KeyStringSet actualKeys;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);

    BSONObj indexSpec = fromjson("{a: 'hashed'}");
    ExpressionKeysPrivate::getHashKeys(obj,
                                       indexSpec,
                                       kHashSeed,
                                       kHashVersion,
                                       false,
                                       &collator,
                                       &actualKeys,
                                       KeyString::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       false);

    KeyStringSet expectedKeys;
    expectedKeys.insert(makeHashKey(backwardsObj["a"]));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(HashKeyGeneratorTest, CollationAppliedforAllIndexFields) {
    BSONObj obj = fromjson("{a: {b: 'abc', c: 'def'}}");
    BSONObj backwardsObj = fromjson("{a: {b: 'cba', c: 'fed'}}");
    KeyStringSet actualKeys;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);

    BSONObj indexSpec = fromjson("{'a.c': 1, 'a': 'hashed'}");
    ExpressionKeysPrivate::getHashKeys(obj,
                                       indexSpec,
                                       kHashSeed,
                                       kHashVersion,
                                       true,
                                       &collator,
                                       &actualKeys,
                                       KeyString::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       false);

    KeyStringSet expectedKeys;
    KeyString::HeapBuilder keyString(KeyString::Version::kLatestVersion, Ordering::make(BSONObj()));
    keyString.appendBSONElement(backwardsObj["a"]["c"]);
    keyString.appendNumberLong(BSONElementHasher::hash64(backwardsObj["a"], kHashSeed));
    expectedKeys.insert(keyString.release());
    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(HashKeyGeneratorTest, NoCollation) {
    BSONObj obj = fromjson("{a: 'string'}");
    KeyStringSet actualKeys;
    BSONObj indexSpec = fromjson("{a: 'hashed'}");
    ExpressionKeysPrivate::getHashKeys(obj,
                                       indexSpec,
                                       kHashSeed,
                                       kHashVersion,
                                       false,
                                       nullptr,
                                       &actualKeys,
                                       KeyString::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       false);

    KeyStringSet expectedKeys;
    expectedKeys.insert(makeHashKey(obj["a"]));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(HashKeyGeneratorTest, CompoundIndexEmptyObject) {
    BSONObj obj = fromjson("{}");
    KeyStringSet actualKeys;
    BSONObj indexSpec = fromjson("{a: 'hashed', b: 1, c: 1}");
    ExpressionKeysPrivate::getHashKeys(obj,
                                       indexSpec,
                                       kHashSeed,
                                       kHashVersion,
                                       false,
                                       nullptr,
                                       &actualKeys,
                                       KeyString::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       false);

    // Verify that we inserted null indexes for empty input object.
    KeyStringSet expectedKeys;
    KeyString::HeapBuilder keyString(KeyString::Version::kLatestVersion, Ordering::make(BSONObj()));
    auto nullBSON = BSON("" << BSONNULL);
    auto nullElement = nullBSON.firstElement();
    keyString.appendNumberLong(BSONElementHasher::hash64(nullElement, kHashSeed));
    keyString.appendBSONElement(nullElement);
    keyString.appendBSONElement(nullElement);
    expectedKeys.insert(keyString.release());
    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(HashKeyGeneratorTest, SparseIndex) {
    BSONObj obj = fromjson("{k: 1}");
    KeyStringSet actualKeys;
    BSONObj indexSpec = fromjson("{a: 'hashed', b: 1, c: 1}");
    ExpressionKeysPrivate::getHashKeys(obj,
                                       indexSpec,
                                       kHashSeed,
                                       kHashVersion,
                                       true,  // isSparse
                                       nullptr,
                                       &actualKeys,
                                       KeyString::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       false);
    // Verify that no index entries were added to the sparse index.
    ASSERT(assertKeysetsEqual(KeyStringSet(), actualKeys));
}

TEST(HashKeyGeneratorTest, SparseIndexWithAFieldPresent) {
    BSONObj obj = fromjson("{a: 2}");
    KeyStringSet actualKeys;
    BSONObj indexSpec = fromjson("{a: 'hashed', b: 1, c: 1}");
    ExpressionKeysPrivate::getHashKeys(obj,
                                       indexSpec,
                                       kHashSeed,
                                       kHashVersion,
                                       true,  // isSparse
                                       nullptr,
                                       &actualKeys,
                                       KeyString::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       false);

    // Verify that we inserted null entries for the misssing fields.
    KeyStringSet expectedKeys;
    KeyString::HeapBuilder keyString(KeyString::Version::kLatestVersion, Ordering::make(BSONObj()));
    auto nullBSON = BSON("" << BSONNULL);
    auto nullElement = nullBSON.firstElement();
    keyString.appendNumberLong(BSONElementHasher::hash64(obj["a"], kHashSeed));
    keyString.appendBSONElement(nullElement);
    keyString.appendBSONElement(nullElement);
    expectedKeys.insert(keyString.release());
    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(HashKeyGeneratorTest, ArrayAlongIndexFieldPathFails) {
    BSONObj obj = fromjson("{a: []}");
    KeyStringSet actualKeys;
    BSONObj indexSpec = fromjson("{'a.b.c': 'hashed'}");
    ASSERT_THROWS_CODE(ExpressionKeysPrivate::getHashKeys(obj,
                                                          indexSpec,
                                                          kHashSeed,
                                                          kHashVersion,
                                                          false,
                                                          nullptr,
                                                          &actualKeys,
                                                          KeyString::Version::kLatestVersion,
                                                          Ordering::make(BSONObj()),
                                                          false),
                       DBException,
                       16766);
}

TEST(HashKeyGeneratorTest, ArrayAlongIndexFieldPathDoesNotFailWhenIgnoreFlagIsSet) {
    BSONObj obj = fromjson("{a: []}");
    KeyStringSet actualKeys;
    BSONObj indexSpec = fromjson("{'a.b.c': 'hashed'}");
    ExpressionKeysPrivate::getHashKeys(obj,
                                       indexSpec,
                                       kHashSeed,
                                       kHashVersion,
                                       false,
                                       nullptr,
                                       &actualKeys,
                                       KeyString::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       true  // ignoreArraysAlongPath
    );

    KeyStringSet expectedKeys;
    expectedKeys.insert(makeHashKey(BSON("" << BSONNULL).firstElement()));
    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(HashKeyGeneratorTest, ArrayAtTerminalPathAlwaysFails) {
    BSONObj obj = fromjson("{a : {b: {c: [1]}}}");
    KeyStringSet actualKeys;
    BSONObj indexSpec = fromjson("{'a.b.c': 'hashed'}");
    ASSERT_THROWS_CODE(ExpressionKeysPrivate::getHashKeys(obj,
                                                          indexSpec,
                                                          kHashSeed,
                                                          kHashVersion,
                                                          true,  // isSparse
                                                          nullptr,
                                                          &actualKeys,
                                                          KeyString::Version::kLatestVersion,
                                                          Ordering::make(BSONObj()),
                                                          true,  // ignoreArraysAlongPath
                                                          boost::none),
                       DBException,
                       16766);
}


}  // namespace
