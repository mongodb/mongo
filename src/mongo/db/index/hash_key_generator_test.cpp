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


#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <algorithm>
#include <ostream>
#include <string>

#include <boost/container/flat_set.hpp>
#include <boost/container/vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


using namespace mongo;

namespace {

const int kHashVersion = 0;

std::string dumpKeyset(const KeyStringSet& keyStrings) {
    std::stringstream ss;
    ss << "[ ";
    for (auto& keyString : keyStrings) {
        auto key = key_string::toBson(keyString, Ordering::make(BSONObj()));
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

key_string::Value makeHashKey(BSONElement elt) {
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion,
        BSON("" << BSONElementHasher::hash64(elt, BSONElementHasher::DEFAULT_HASH_SEED)),
        Ordering::make(BSONObj()));
    return keyString.release();
}

struct HashKeyGeneratorTest : public unittest::Test {
    SharedBufferFragmentBuilder allocator{key_string::HeapBuilder::kHeapAllocatorDefaultBytes};
};

TEST_F(HashKeyGeneratorTest, CollationAppliedBeforeHashing) {
    BSONObj obj = fromjson("{a: 'string'}");
    KeyStringSet actualKeys;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    BSONObj indexSpec = fromjson("{a: 'hashed'}");
    ExpressionKeysPrivate::getHashKeys(allocator,
                                       obj,
                                       indexSpec,
                                       kHashVersion,
                                       false,
                                       &collator,
                                       &actualKeys,
                                       key_string::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       false);

    BSONObj backwardsObj = fromjson("{a: 'gnirts'}");
    KeyStringSet expectedKeys;
    expectedKeys.insert(makeHashKey(backwardsObj["a"]));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST_F(HashKeyGeneratorTest, CollationDoesNotAffectNonStringFields) {
    BSONObj obj = fromjson("{a: 5}");
    KeyStringSet actualKeys;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);

    BSONObj indexSpec = fromjson("{a: 'hashed'}");
    ExpressionKeysPrivate::getHashKeys(allocator,
                                       obj,
                                       indexSpec,
                                       kHashVersion,
                                       false,
                                       &collator,
                                       &actualKeys,
                                       key_string::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       false);

    KeyStringSet expectedKeys;
    expectedKeys.insert(makeHashKey(obj["a"]));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST_F(HashKeyGeneratorTest, CollatorAppliedBeforeHashingNestedObject) {
    BSONObj obj = fromjson("{a: {b: 'string'}}");
    BSONObj backwardsObj = fromjson("{a: {b: 'gnirts'}}");
    KeyStringSet actualKeys;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);

    BSONObj indexSpec = fromjson("{a: 'hashed'}");
    ExpressionKeysPrivate::getHashKeys(allocator,
                                       obj,
                                       indexSpec,
                                       kHashVersion,
                                       false,
                                       &collator,
                                       &actualKeys,
                                       key_string::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       false);

    KeyStringSet expectedKeys;
    expectedKeys.insert(makeHashKey(backwardsObj["a"]));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST_F(HashKeyGeneratorTest, CollationAppliedforAllIndexFields) {
    BSONObj obj = fromjson("{a: {b: 'abc', c: 'def'}}");
    BSONObj backwardsObj = fromjson("{a: {b: 'cba', c: 'fed'}}");
    KeyStringSet actualKeys;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);

    BSONObj indexSpec = fromjson("{'a.c': 1, 'a': 'hashed'}");
    ExpressionKeysPrivate::getHashKeys(allocator,
                                       obj,
                                       indexSpec,
                                       kHashVersion,
                                       true,
                                       &collator,
                                       &actualKeys,
                                       key_string::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       false);

    KeyStringSet expectedKeys;
    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      Ordering::make(BSONObj()));
    keyString.appendBSONElement(backwardsObj["a"]["c"]);
    keyString.appendNumberLong(
        BSONElementHasher::hash64(backwardsObj["a"], BSONElementHasher::DEFAULT_HASH_SEED));
    expectedKeys.insert(keyString.release());
    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST_F(HashKeyGeneratorTest, NoCollation) {
    BSONObj obj = fromjson("{a: 'string'}");
    KeyStringSet actualKeys;
    BSONObj indexSpec = fromjson("{a: 'hashed'}");
    ExpressionKeysPrivate::getHashKeys(allocator,
                                       obj,
                                       indexSpec,
                                       kHashVersion,
                                       false,
                                       nullptr,
                                       &actualKeys,
                                       key_string::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       false);

    KeyStringSet expectedKeys;
    expectedKeys.insert(makeHashKey(obj["a"]));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST_F(HashKeyGeneratorTest, CompoundIndexEmptyObject) {
    BSONObj obj = fromjson("{}");
    KeyStringSet actualKeys;
    BSONObj indexSpec = fromjson("{a: 'hashed', b: 1, c: 1}");
    ExpressionKeysPrivate::getHashKeys(allocator,
                                       obj,
                                       indexSpec,
                                       kHashVersion,
                                       false,
                                       nullptr,
                                       &actualKeys,
                                       key_string::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       false);

    // Verify that we inserted null indexes for empty input object.
    KeyStringSet expectedKeys;
    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      Ordering::make(BSONObj()));
    auto nullBSON = BSON("" << BSONNULL);
    auto nullElement = nullBSON.firstElement();
    keyString.appendNumberLong(
        BSONElementHasher::hash64(nullElement, BSONElementHasher::DEFAULT_HASH_SEED));
    keyString.appendBSONElement(nullElement);
    keyString.appendBSONElement(nullElement);
    expectedKeys.insert(keyString.release());
    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST_F(HashKeyGeneratorTest, SparseIndex) {
    BSONObj obj = fromjson("{k: 1}");
    KeyStringSet actualKeys;
    BSONObj indexSpec = fromjson("{a: 'hashed', b: 1, c: 1}");
    ExpressionKeysPrivate::getHashKeys(allocator,
                                       obj,
                                       indexSpec,
                                       kHashVersion,
                                       true,  // isSparse
                                       nullptr,
                                       &actualKeys,
                                       key_string::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       false);
    // Verify that no index entries were added to the sparse index.
    ASSERT(assertKeysetsEqual(KeyStringSet(), actualKeys));
}

TEST_F(HashKeyGeneratorTest, SparseIndexWithAFieldPresent) {
    BSONObj obj = fromjson("{a: 2}");
    KeyStringSet actualKeys;
    BSONObj indexSpec = fromjson("{a: 'hashed', b: 1, c: 1}");
    ExpressionKeysPrivate::getHashKeys(allocator,
                                       obj,
                                       indexSpec,
                                       kHashVersion,
                                       true,  // isSparse
                                       nullptr,
                                       &actualKeys,
                                       key_string::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       false);

    // Verify that we inserted null entries for the misssing fields.
    KeyStringSet expectedKeys;
    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      Ordering::make(BSONObj()));
    auto nullBSON = BSON("" << BSONNULL);
    auto nullElement = nullBSON.firstElement();
    keyString.appendNumberLong(
        BSONElementHasher::hash64(obj["a"], BSONElementHasher::DEFAULT_HASH_SEED));
    keyString.appendBSONElement(nullElement);
    keyString.appendBSONElement(nullElement);
    expectedKeys.insert(keyString.release());
    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST_F(HashKeyGeneratorTest, ArrayAlongIndexFieldPathFails) {
    BSONObj obj = fromjson("{a: []}");
    KeyStringSet actualKeys;
    BSONObj indexSpec = fromjson("{'a.b.c': 'hashed'}");
    ASSERT_THROWS_CODE(ExpressionKeysPrivate::getHashKeys(allocator,
                                                          obj,
                                                          indexSpec,
                                                          kHashVersion,
                                                          false,
                                                          nullptr,
                                                          &actualKeys,
                                                          key_string::Version::kLatestVersion,
                                                          Ordering::make(BSONObj()),
                                                          false),
                       DBException,
                       16766);
}

TEST_F(HashKeyGeneratorTest, ArrayAlongIndexFieldPathDoesNotFailWhenIgnoreFlagIsSet) {
    BSONObj obj = fromjson("{a: []}");
    KeyStringSet actualKeys;
    BSONObj indexSpec = fromjson("{'a.b.c': 'hashed'}");
    ExpressionKeysPrivate::getHashKeys(allocator,
                                       obj,
                                       indexSpec,
                                       kHashVersion,
                                       false,
                                       nullptr,
                                       &actualKeys,
                                       key_string::Version::kLatestVersion,
                                       Ordering::make(BSONObj()),
                                       true  // ignoreArraysAlongPath
    );

    KeyStringSet expectedKeys;
    expectedKeys.insert(makeHashKey(BSON("" << BSONNULL).firstElement()));
    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST_F(HashKeyGeneratorTest, ArrayAtTerminalPathAlwaysFails) {
    BSONObj obj = fromjson("{a : {b: {c: [1]}}}");
    KeyStringSet actualKeys;
    BSONObj indexSpec = fromjson("{'a.b.c': 'hashed'}");
    ASSERT_THROWS_CODE(ExpressionKeysPrivate::getHashKeys(allocator,
                                                          obj,
                                                          indexSpec,
                                                          kHashVersion,
                                                          true,  // isSparse
                                                          nullptr,
                                                          &actualKeys,
                                                          key_string::Version::kLatestVersion,
                                                          Ordering::make(BSONObj()),
                                                          true,  // ignoreArraysAlongPath
                                                          boost::none),
                       DBException,
                       16766);
}


}  // namespace
