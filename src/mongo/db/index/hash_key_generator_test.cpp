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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/index/expression_keys_private.h"

#include <algorithm>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/hasher.h"
#include "mongo/db/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

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
        log() << "Expected: " << dumpKeyset(expectedKeys) << ", "
              << "Actual: " << dumpKeyset(actualKeys);
        return false;
    }

    if (!std::equal(expectedKeys.begin(), expectedKeys.end(), actualKeys.begin())) {
        log() << "Expected: " << dumpKeyset(expectedKeys) << ", "
              << "Actual: " << dumpKeyset(actualKeys);
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
    ExpressionKeysPrivate::getHashKeys(obj,
                                       "a",
                                       kHashSeed,
                                       kHashVersion,
                                       false,
                                       &collator,
                                       &actualKeys,
                                       KeyString::Version::kLatestVersion,
                                       Ordering::make(BSONObj()));

    BSONObj backwardsObj = fromjson("{a: 'gnirts'}");
    KeyStringSet expectedKeys;
    expectedKeys.insert(makeHashKey(backwardsObj["a"]));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(HashKeyGeneratorTest, CollationDoesNotAffectNonStringFields) {
    BSONObj obj = fromjson("{a: 5}");
    KeyStringSet actualKeys;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionKeysPrivate::getHashKeys(obj,
                                       "a",
                                       kHashSeed,
                                       kHashVersion,
                                       false,
                                       &collator,
                                       &actualKeys,
                                       KeyString::Version::kLatestVersion,
                                       Ordering::make(BSONObj()));

    KeyStringSet expectedKeys;
    expectedKeys.insert(makeHashKey(obj["a"]));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(HashKeyGeneratorTest, CollatorAppliedBeforeHashingNestedObject) {
    BSONObj obj = fromjson("{a: {b: 'string'}}");
    BSONObj backwardsObj = fromjson("{a: {b: 'gnirts'}}");
    KeyStringSet actualKeys;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionKeysPrivate::getHashKeys(obj,
                                       "a",
                                       kHashSeed,
                                       kHashVersion,
                                       false,
                                       &collator,
                                       &actualKeys,
                                       KeyString::Version::kLatestVersion,
                                       Ordering::make(BSONObj()));

    KeyStringSet expectedKeys;
    expectedKeys.insert(makeHashKey(backwardsObj["a"]));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(HashKeyGeneratorTest, NoCollation) {
    BSONObj obj = fromjson("{a: 'string'}");
    KeyStringSet actualKeys;
    ExpressionKeysPrivate::getHashKeys(obj,
                                       "a",
                                       kHashSeed,
                                       kHashVersion,
                                       false,
                                       nullptr,
                                       &actualKeys,
                                       KeyString::Version::kLatestVersion,
                                       Ordering::make(BSONObj()));

    KeyStringSet expectedKeys;
    expectedKeys.insert(makeHashKey(obj["a"]));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

}  // namespace
