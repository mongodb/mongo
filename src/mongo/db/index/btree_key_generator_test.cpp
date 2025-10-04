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


#include "mongo/db/index/btree_key_generator.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


using namespace mongo;
using std::vector;

namespace {

//
// Helper functions
//

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

std::string dumpMultikeyPaths(const MultikeyPaths& multikeyPaths) {
    std::stringstream ss;

    ss << "[ ";
    for (const auto& multikeyComponents : multikeyPaths) {
        ss << "[ ";
        for (const auto& multikeyComponent : multikeyComponents) {
            ss << multikeyComponent << " ";
        }
        ss << "] ";
    }
    ss << "]";

    return ss.str();
}

bool keysetsEqual(const KeyStringSet& expectedKeys, const KeyStringSet& actualKeys) {
    if (expectedKeys.size() != actualKeys.size()) {
        return false;
    }

    if (!std::equal(expectedKeys.begin(), expectedKeys.end(), actualKeys.begin())) {
        return false;
    }

    return true;
}

bool testKeygen(const BSONObj& kp,
                const BSONObj& obj,
                const KeyStringSet& expectedKeys,
                const MultikeyPaths& expectedMultikeyPaths,
                bool sparse = false,
                const CollatorInterface* collator = nullptr,
                bool runWithoutSkipMultiKey = false) {
    invariant(expectedMultikeyPaths.size() == static_cast<size_t>(kp.nFields()));

    //
    // Construct the btree key generator object, using the index key pattern.
    //
    vector<const char*> fieldNames;
    vector<BSONElement> fixed;

    BSONObjIterator it(kp);
    while (it.more()) {
        BSONElement elt = it.next();
        fieldNames.push_back(elt.fieldName());
        fixed.push_back(BSONElement());
    }

    auto keyGen = std::make_unique<BtreeKeyGenerator>(
        fieldNames, fixed, sparse, key_string::Version::kLatestVersion, Ordering::make(BSONObj()));

    auto runTest = [&](bool skipMultikey) {
        //
        // Ask 'keyGen' to generate index keys for the object 'obj' and report any prefixes of the
        // indexed fields that would cause the index to be multikey as a result of inserting
        // 'actualKeys'.
        //
        SharedBufferFragmentBuilder allocator(BufBuilder::kDefaultInitSizeBytes);
        KeyStringSet actualKeys;
        MultikeyPaths actualMultikeyPaths;
        keyGen->getKeys(allocator, obj, skipMultikey, &actualKeys, &actualMultikeyPaths, collator);

        //
        // Check that the results match the expected result.
        //
        bool match = keysetsEqual(expectedKeys, actualKeys);
        if (!match) {
            LOGV2(20647,
                  "Expected: {dumpKeyset_expectedKeys}, Actual: {dumpKeyset_actualKeys}",
                  "dumpKeyset_expectedKeys"_attr = dumpKeyset(expectedKeys),
                  "dumpKeyset_actualKeys"_attr = dumpKeyset(actualKeys));
            return false;
        }

        match = (expectedMultikeyPaths == actualMultikeyPaths);
        if (!match) {
            LOGV2(20648,
                  "Expected: {dumpMultikeyPaths_expectedMultikeyPaths}, Actual: "
                  "{dumpMultikeyPaths_actualMultikeyPaths}",
                  "dumpMultikeyPaths_expectedMultikeyPaths"_attr =
                      dumpMultikeyPaths(expectedMultikeyPaths),
                  "dumpMultikeyPaths_actualMultikeyPaths"_attr =
                      dumpMultikeyPaths(actualMultikeyPaths));
            return false;
        }

        return true;
    };

    // If it is correct to do so, then test that the fast key generation path for the non-multikey
    // case works as expected.
    if (!runWithoutSkipMultiKey &&
        std::all_of(expectedMultikeyPaths.begin(),
                    expectedMultikeyPaths.end(),
                    [](const auto& path) { return path.empty(); })) {
        if (!runTest(true)) {
            return false;
        }
    }

    // Test that fully general key generation path works as expected.
    return runTest(false);
}

//
// Unit tests
//


TEST(BtreeKeyGeneratorTest, GetIdKeyFromObject) {
    BSONObj keyPattern = fromjson("{_id: 1}");
    BSONObj genKeysFrom = fromjson("{_id: 'foo', b: 4}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 'foo'}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromObjectSimple) {
    BSONObj keyPattern = fromjson("{a: 1}");
    BSONObj genKeysFrom = fromjson("{b: 4, a: 5}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 5}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromObjectDotted) {
    BSONObj keyPattern = fromjson("{'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: {b: 4}, c: 'foo'}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 4}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromArraySimple) {
    BSONObj keyPattern = fromjson("{a: 1}");
    BSONObj genKeysFrom = fromjson("{a: [1, 2, 3]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(
        key_string::Version::kLatestVersion, fromjson("{'': 3}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromArrayWithIdenticalValues) {
    BSONObj keyPattern = fromjson("{a: 1}");
    BSONObj genKeysFrom = fromjson("{a: [0, 0, 0]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 0}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromArrayWithEquivalentValues) {
    BSONObj keyPattern = fromjson("{a: 1}");
    BSONObj genKeysFrom = fromjson("{a: [0, NumberInt(0), NumberLong(0)]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 0}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromArrayFirstElement) {
    BSONObj keyPattern = fromjson("{a: 1, b: 1}");
    BSONObj genKeysFrom = fromjson("{a: [1, 2, 3], b: 2}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1, '': 2}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 2, '': 2}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(
        key_string::Version::kLatestVersion, fromjson("{'': 3, '': 2}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}, MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromArraySecondElement) {
    BSONObj keyPattern = fromjson("{first: 1, a: 1}");
    BSONObj genKeysFrom = fromjson("{first: 5, a: [1, 2, 3]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 5, '': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 5, '': 2}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(
        key_string::Version::kLatestVersion, fromjson("{'': 5, '': 3}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}, {0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromSecondLevelArray) {
    BSONObj keyPattern = fromjson("{'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: {b: [1, 2, 3]}}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(
        key_string::Version::kLatestVersion, fromjson("{'': 3}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};
    MultikeyPaths expectedMultikeyPaths{{1U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromParallelArraysBasic) {
    BSONObj keyPattern = fromjson("{'a': 1, 'b': 1}");
    BSONObj genKeysFrom = fromjson("{a: [1, 2, 3], b: [1, 2, 3]}");
    KeyStringSet expectedKeys;
    MultikeyPaths expectedMultikeyPaths(keyPattern.nFields());
    ASSERT_THROWS(testKeygen(keyPattern,
                             genKeysFrom,
                             expectedKeys,
                             expectedMultikeyPaths,
                             false,    // sparse
                             nullptr,  // collator
                             true),    // runWithoutSkipMultiKey
                  AssertionException);
}

TEST(BtreeKeyGeneratorTest, GetKeysFromArraySubobjectBasic) {
    BSONObj keyPattern = fromjson("{'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: [{b:1,c:4}, {b:2,c:4}, {b:3,c:4}]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(
        key_string::Version::kLatestVersion, fromjson("{'': 3}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromSubobjectWithArrayOfSubobjects) {
    BSONObj keyPattern = fromjson("{'a.b.c': 1}");
    BSONObj genKeysFrom = fromjson("{a: {b: [{c: 1}, {c: 2}]}}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release()};
    MultikeyPaths expectedMultikeyPaths{{1U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysArraySubobjectCompoundIndex) {
    BSONObj keyPattern = fromjson("{'a.b': 1, d: 99}");
    BSONObj genKeysFrom = fromjson("{a: [{b:1,c:4}, {b:2,c:4}, {b:3,c:4}], d: 99}");
    key_string::HeapBuilder keyString1(key_string::Version::kLatestVersion,
                                       fromjson("{'': 1, '': 99}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(key_string::Version::kLatestVersion,
                                       fromjson("{'': 2, '': 99}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(key_string::Version::kLatestVersion,
                                       fromjson("{'': 3, '': 99}"),
                                       Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}, MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysArraySubobjectSingleMissing) {
    BSONObj keyPattern = fromjson("{'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: [{foo: 41}, {b:1,c:4}, {b:2,c:4}, {b:3,c:4}]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': null}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString4(
        key_string::Version::kLatestVersion, fromjson("{'': 3}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{
        keyString1.release(), keyString2.release(), keyString3.release(), keyString4.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromArraySubobjectMissing) {
    BSONObj keyPattern = fromjson("{'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: [{foo: 41}, {foo: 41}, {foo: 41}]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': null}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysMissingField) {
    BSONObj keyPattern = fromjson("{a: 1}");
    BSONObj genKeysFrom = fromjson("{b: 1}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': null}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysSubobjectMissing) {
    BSONObj keyPattern = fromjson("{'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: [1, 2]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': null}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromCompound) {
    BSONObj keyPattern = fromjson("{x: 1, y: 1}");
    BSONObj genKeysFrom = fromjson("{x: 'a', y: 'b'}");
    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      fromjson("{'': 'a', '': 'b'}"),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}, MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromCompoundMissing) {
    BSONObj keyPattern = fromjson("{x: 1, y: 1}");
    BSONObj genKeysFrom = fromjson("{x: 'a'}");
    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      fromjson("{'': 'a', '': null}"),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}, MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromArraySubelementComplex) {
    BSONObj keyPattern = fromjson("{'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a:[{b:[2]}]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    // Both the 'a' and 'a.b' arrays contain a single element, so they are considered multikey.
    MultikeyPaths expectedMultikeyPaths{{0U, 1U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromParallelArraysComplex) {
    BSONObj keyPattern = fromjson("{'a.b': 1, 'a.c': 1}");
    BSONObj genKeysFrom = fromjson("{a:[{b:[1],c:[2]}]}");
    KeyStringSet expectedKeys;
    MultikeyPaths expectedMultikeyPaths(keyPattern.nFields());
    ASSERT_THROWS(testKeygen(keyPattern,
                             genKeysFrom,
                             expectedKeys,
                             expectedMultikeyPaths,
                             false,    // sparse
                             nullptr,  // collator
                             true),    // runWithoutSkipMultiKey
                  AssertionException);
}

TEST(BtreeKeyGeneratorTest, GetKeysAlternateMissing) {
    BSONObj keyPattern = fromjson("{'a.b': 1, 'a.c': 1}");
    BSONObj genKeysFrom = fromjson("{a:[{b:1},{c:2}]}");
    key_string::HeapBuilder keyString1(key_string::Version::kLatestVersion,
                                       fromjson("{'': null, '': 2}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(key_string::Version::kLatestVersion,
                                       fromjson("{'': 1, '': null}"),
                                       Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}, {0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromMultiComplex) {
    BSONObj keyPattern = fromjson("{'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a:[{b:1},{b:[1,2,3]}]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(
        key_string::Version::kLatestVersion, fromjson("{'': 3}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};
    MultikeyPaths expectedMultikeyPaths{{0U, 1U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromArrayOfSubobjectsWithArrayValues) {
    BSONObj keyPattern = fromjson("{'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: [{b: [1, 2]}, {b: [2, 3]}]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(
        key_string::Version::kLatestVersion, fromjson("{'': 3}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};
    MultikeyPaths expectedMultikeyPaths{{0U, 1U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromArrayOfSubobjectsWithNonDistinctArrayValues) {
    BSONObj keyPattern = fromjson("{'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: [{b: [1, 2, 3]}, {b: [2]}, {b: [3, 1]}]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(
        key_string::Version::kLatestVersion, fromjson("{'': 3}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};
    MultikeyPaths expectedMultikeyPaths{{0U, 1U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysArrayEmpty) {
    BSONObj keyPattern = fromjson("{a: 1}");
    BSONObj genKeysFrom = fromjson("{a:[1,2]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(key_string::Version::kLatestVersion,
                                       fromjson("{'': undefined}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString4(
        key_string::Version::kLatestVersion, fromjson("{'': null}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.getValueCopy(), keyString2.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a: [1]}");
    expectedKeys.clear();
    expectedKeys.insert(keyString1.release());
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a: []}");
    expectedKeys.clear();
    expectedKeys.insert(keyString3.release());
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a: null}");
    expectedKeys.clear();
    expectedKeys.insert(keyString4.release());
    expectedMultikeyPaths[0].clear();
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromDoubleArray) {
    BSONObj keyPattern = fromjson("{a: 1, a: 1}");
    BSONObj genKeysFrom = fromjson("{a:[1,2]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1, '': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 2, '': 2}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}, {0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromDoubleEmptyArray) {
    BSONObj keyPattern = fromjson("{a: 1, a: 1}");
    BSONObj genKeysFrom = fromjson("{a:[]}");
    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      fromjson("{'': undefined, '': undefined}"),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}, {0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromMultiEmptyArray) {
    BSONObj keyPattern = fromjson("{a: 1, b: 1}");
    BSONObj genKeysFrom = fromjson("{a: 1, b: [1, 2]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1, '': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 1, '': 2}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(key_string::Version::kLatestVersion,
                                       fromjson("{'': 1, '': undefined}"),
                                       Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.getValueCopy(), keyString2.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}, {0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a: 1, b: [1]}");
    expectedKeys.clear();
    expectedKeys.insert(keyString1.release());
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a: 1, b: []}");
    expectedKeys.clear();
    expectedKeys.insert(keyString3.release());
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromNestedEmptyArray) {
    BSONObj keyPattern = fromjson("{'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a:[]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': null}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromMultiNestedEmptyArray) {
    BSONObj keyPattern = fromjson("{'a.b': 1, 'a.c': 1}");
    BSONObj genKeysFrom = fromjson("{a:[]}");
    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      fromjson("{'': null, '': null}"),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}, {0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromUnevenNestedEmptyArray) {
    BSONObj keyPattern = fromjson("{'a': 1, 'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a:[]}");
    key_string::HeapBuilder keyString1(key_string::Version::kLatestVersion,
                                       fromjson("{'': undefined, '': null}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(key_string::Version::kLatestVersion,
                                       fromjson("{'': {b:1}, '': 1}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(key_string::Version::kLatestVersion,
                                       fromjson("{'': {b:[]}, '': undefined}"),
                                       Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}, {0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a:[{b: 1}]}");
    expectedKeys.clear();
    expectedKeys.insert(keyString2.release());
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a:[{b: []}]}");
    expectedKeys.clear();
    expectedKeys.insert(keyString3.release());
    expectedMultikeyPaths[1].insert(1U);
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromReverseUnevenNestedEmptyArray) {
    BSONObj keyPattern = fromjson("{'a.b': 1, 'a': 1}");
    BSONObj genKeysFrom = fromjson("{a:[]}");
    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      fromjson("{'': null, '': undefined}"),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}, {0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, SparseReverseUnevenNestedEmptyArray) {
    const bool sparse = true;
    BSONObj keyPattern = fromjson("{'a.b': 1, 'a': 1}");
    BSONObj genKeysFrom = fromjson("{a:[]}");
    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      fromjson("{'': null, '': undefined}"),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}, {0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, sparse));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromSparseEmptyArray) {
    const bool sparse = true;
    BSONObj keyPattern = fromjson("{'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a:1}");
    KeyStringSet expectedKeys;
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, sparse));

    genKeysFrom = fromjson("{a:[]}");
    expectedMultikeyPaths[0].insert(0U);
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, sparse));

    genKeysFrom = fromjson("{a:[{c:1}]}");
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, sparse));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromSparseEmptyArraySecond) {
    const bool sparse = true;
    BSONObj keyPattern = fromjson("{z: 1, 'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a:1}");
    KeyStringSet expectedKeys;
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}, MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, sparse));

    genKeysFrom = fromjson("{a:[]}");
    expectedMultikeyPaths[1].insert(0U);
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, sparse));

    genKeysFrom = fromjson("{a:[{c:1}]}");
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, sparse));
}

TEST(BtreeKeyGeneratorTest, SparseNonObjectMissingNestedField) {
    const bool sparse = true;
    BSONObj keyPattern = fromjson("{'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a:[]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys;
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, sparse));

    genKeysFrom = fromjson("{a:[1]}");
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, sparse));

    genKeysFrom = fromjson("{a:[1,{b:1}]}");
    expectedKeys.clear();
    expectedKeys.insert(keyString.release());
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, sparse));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromIndexedArrayIndex) {
    BSONObj keyPattern = fromjson("{'a.0': 1}");
    BSONObj genKeysFrom = fromjson("{a:[1]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': [1]}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(key_string::Version::kLatestVersion,
                                       fromjson("{'': undefined}"),
                                       Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.getValueCopy()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a:[[1]]}");
    expectedKeys.clear();
    expectedKeys.insert(keyString2.release());
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a:[[]]}");
    expectedKeys.clear();
    expectedKeys.insert(keyString3.getValueCopy());
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a:[[]]}");
    expectedKeys.clear();
    expectedKeys.insert(keyString3.release());
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a:{'0':1}}");
    expectedKeys.clear();
    expectedKeys.insert(keyString1.release());
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a:[{'0':1}]}");
    expectedKeys.clear();
    ASSERT_THROWS(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths),
                  AssertionException);

    genKeysFrom = fromjson("{a:[1,{'0':2}]}");
    ASSERT_THROWS(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths),
                  AssertionException);
}

TEST(BtreeKeyGeneratorTest, GetKeysFromDoubleIndexedArrayIndex) {
    BSONObj keyPattern = fromjson("{'a.0.0': 1}");
    BSONObj genKeysFrom = fromjson("{a:[[1]]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': null}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(key_string::Version::kLatestVersion,
                                       fromjson("{'': undefined}"),
                                       Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a:[[]]}");
    expectedKeys.clear();
    expectedKeys.insert(keyString2.getValueCopy());
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a:[]}");
    expectedKeys.clear();
    expectedKeys.insert(keyString2.release());
    // Here the first "0" path component acts like a regular field name rather than a positional
    // path element, since the 0th array index does not exist. Therefore, path component "a" is
    // considered multikey.
    expectedMultikeyPaths = {{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a:[[[]]]}");
    expectedKeys.clear();
    expectedKeys.insert(keyString3.release());
    expectedMultikeyPaths = {MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromObjectWithinArray) {
    BSONObj keyPattern = fromjson("{'a.0.b': 1}");
    BSONObj genKeysFrom = fromjson("{a:[{b:1}]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': [1]}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(key_string::Version::kLatestVersion,
                                       fromjson("{'': undefined}"),
                                       Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.getValueCopy()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a:[{b:[1]}]}");
    expectedMultikeyPaths = {{2U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a:[{b:[[1]]}]}");
    expectedKeys.clear();
    expectedKeys.insert(keyString2.getValueCopy());
    expectedMultikeyPaths = {{2U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a:[[{b:1}]]}");
    expectedKeys.clear();
    expectedKeys.insert(keyString1.release());
    // Path component "0" refers to an array which is subsequently expanded, so it is considered
    // multikey.
    expectedMultikeyPaths = {{1U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a:[[{b:[1]}]]}");
    expectedMultikeyPaths = {{1U, 2U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a:[[{b:[[1]]}]]}");
    expectedKeys.clear();
    expectedKeys.insert(keyString2.release());
    expectedMultikeyPaths = {{1U, 2U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a:[[{b:[]}]]}");
    expectedKeys.clear();
    expectedKeys.insert(keyString3.release());
    expectedMultikeyPaths = {{1U, 2U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysPositionalElementIsExpandedArray) {
    BSONObj keyPattern = fromjson("{'a.0.b': 1}");
    BSONObj genKeysFrom = fromjson("{a:[[{b:1}, {b:2}]]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release()};
    MultikeyPaths expectedMultikeyPaths{{1U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysTrailingPositionalElementIsSingletonArray) {
    BSONObj keyPattern = fromjson("{'a.b.c.3': 1}");
    BSONObj genKeysFrom = fromjson("{a:{b:{c:[0,1,2,[3]]}}}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': [3]}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysTrailingPositionalElementIsEmptyArray) {
    BSONObj keyPattern = fromjson("{'a.b.c.3': 1}");
    BSONObj genKeysFrom = fromjson("{a:{b:{c:[0,1,2,[]]}}}");
    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      fromjson("{'': undefined}"),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysManyPositionalElementsComplex) {
    BSONObj keyPattern = fromjson("{'a.0.1.2.b.0': 1}");
    BSONObj genKeysFrom = fromjson("{a:[[1, [1, 2, [{b: [[], 2]}]]], 1]}");
    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      fromjson("{'': undefined}"),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{3U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFromArrayWithinObjectWithinArray) {
    BSONObj keyPattern = fromjson("{'a.0.b.0': 1}");
    BSONObj genKeysFrom = fromjson("{a:[{b:[1]}]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, ParallelArraysInNestedObjects) {
    BSONObj keyPattern = fromjson("{'a.a': 1, 'b.a': 1}");
    BSONObj genKeysFrom = fromjson("{a:{a:[1]}, b:{a:[1]}}");
    KeyStringSet expectedKeys;
    MultikeyPaths expectedMultikeyPaths(keyPattern.nFields());
    ASSERT_THROWS(testKeygen(keyPattern,
                             genKeysFrom,
                             expectedKeys,
                             expectedMultikeyPaths,
                             false,    // sparse
                             nullptr,  // collator
                             true),    // runWithoutSkipMultiKey
                  AssertionException);
}

TEST(BtreeKeyGeneratorTest, ParallelArraysUneven) {
    BSONObj keyPattern = fromjson("{'b.a': 1, 'a': 1}");
    BSONObj genKeysFrom = fromjson("{b:{a:[1]}, a:[1,2]}");
    KeyStringSet expectedKeys;
    MultikeyPaths expectedMultikeyPaths(keyPattern.nFields());
    ASSERT_THROWS(testKeygen(keyPattern,
                             genKeysFrom,
                             expectedKeys,
                             expectedMultikeyPaths,
                             false,    // sparse
                             nullptr,  // collator
                             true),    // runWithoutSkipMultiKey
                  AssertionException);
}

TEST(BtreeKeyGeneratorTest, MultipleArraysNotParallel) {
    BSONObj keyPattern = fromjson("{'a.b.c': 1}");
    BSONObj genKeysFrom = fromjson("{a: [1, 2, {b: {c: [3, 4]}}]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': null}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 3}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(
        key_string::Version::kLatestVersion, fromjson("{'': 4}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};
    MultikeyPaths expectedMultikeyPaths{{0U, 2U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, MultipleArraysNotParallelCompound) {
    BSONObj keyPattern = fromjson("{'a.b.c': 1, 'a.b.d': 1}");
    BSONObj genKeysFrom = fromjson("{a: [1, 2, {b: {c: [3, 4], d: 5}}]}");
    key_string::HeapBuilder keyString1(key_string::Version::kLatestVersion,
                                       fromjson("{'': null, '': null}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 3, '': 5}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(
        key_string::Version::kLatestVersion, fromjson("{'': 4, '': 5}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};
    MultikeyPaths expectedMultikeyPaths{{0U, 2U}, {0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysComplexNestedArrays) {
    BSONObj keyPattern = fromjson("{'a.b.c.d': 1, 'a.g': 1, 'a.b.f': 1, 'a.b.c': 1, 'a.b.e': 1}");
    BSONObj genKeysFrom = fromjson("{a: [1, {b: [2, {c: [3, {d: 1}], e: 4}, 5, {f: 6}], g: 7}]}");
    key_string::HeapBuilder keyString1(key_string::Version::kLatestVersion,
                                       fromjson("{'':null, '':null, '':null, '':null, '':null}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(key_string::Version::kLatestVersion,
                                       fromjson("{'':null, '':7, '':null, '':null, '':null}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(key_string::Version::kLatestVersion,
                                       fromjson("{'':null, '':7, '':null, '':3, '':4}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString4(key_string::Version::kLatestVersion,
                                       fromjson("{'':null, '':7, '':6, '':null, '':null}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString5(key_string::Version::kLatestVersion,
                                       fromjson("{'':1, '':7, '':null, '':{d: 1}, '':4}"),
                                       Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(),
                              keyString2.release(),
                              keyString3.release(),
                              keyString4.release(),
                              keyString5.release()};
    MultikeyPaths expectedMultikeyPaths{{0U, 1U, 2U}, {0U}, {0U, 1U}, {0U, 1U, 2U}, {0U, 1U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

// Descriptive test. Should future index versions recursively index nested arrays?
TEST(BtreeKeyGeneratorTest, GetKeys2DArray) {
    BSONObj keyPattern = fromjson("{a: 1}");
    BSONObj genKeysFrom = fromjson("{a: [[2]]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': [2]}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

// Descriptive test. Should parallel indexed arrays be allowed? If not, should empty
// or single-element arrays be considered for the parallel array check?
TEST(BtreeKeyGeneratorTest, GetKeysParallelEmptyArrays) {
    BSONObj keyPattern = fromjson("{a: 1, b: 1}");
    BSONObj genKeysFrom = fromjson("{a: [], b: []}");
    KeyStringSet expectedKeys;
    MultikeyPaths expectedMultikeyPaths(keyPattern.nFields());
    ASSERT_THROWS(testKeygen(keyPattern,
                             genKeysFrom,
                             expectedKeys,
                             expectedMultikeyPaths,
                             false,    // sparse
                             nullptr,  // collator
                             true),    // runWithoutSkipMultiKey
                  AssertionException);
}

TEST(BtreeKeyGeneratorTest, GetKeysParallelArraysOneArrayEmpty) {
    BSONObj keyPattern = fromjson("{a: 1, b: 1}");
    BSONObj genKeysFrom = fromjson("{a: [], b: [1, 2, 3]}");
    KeyStringSet expectedKeys;
    MultikeyPaths expectedMultikeyPaths(keyPattern.nFields());
    ASSERT_THROWS(testKeygen(keyPattern,
                             genKeysFrom,
                             expectedKeys,
                             expectedMultikeyPaths,
                             false,    // sparse
                             nullptr,  // collator
                             true),    // runWithoutSkipMultiKey
                  AssertionException);
}

TEST(BtreeKeyGeneratorTest, GetKeysParallelArraysOneArrayEmptyNested) {
    BSONObj keyPattern = fromjson("{'a.b.c': 1, 'a.b.d': 1}");
    BSONObj genKeysFrom = fromjson("{a: [{b: [{c: [1, 2, 3], d: []}]}]}");
    KeyStringSet expectedKeys;
    MultikeyPaths expectedMultikeyPaths(keyPattern.nFields());
    ASSERT_THROWS(testKeygen(keyPattern,
                             genKeysFrom,
                             expectedKeys,
                             expectedMultikeyPaths,
                             false,    // sparse
                             nullptr,  // collator
                             true),    // runWithoutSkipMultiKey
                  AssertionException);
}

// Descriptive test. The semantics for key generation are odd for positional key patterns.
TEST(BtreeKeyGeneratorTest, GetKeysPositionalKeyPatternMissingElement) {
    BSONObj keyPattern = fromjson("{'a.2': 1}");
    BSONObj genKeysFrom = fromjson("{a: [{'2': 5}]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 5}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

// Descriptive test. The semantics for key generation are odd for positional key patterns.
TEST(BtreeKeyGeneratorTest, GetKeysPositionalKeyPatternNestedArray) {
    BSONObj keyPattern = fromjson("{'a.2': 1}");
    BSONObj genKeysFrom = fromjson("{a: [[1, 2, 5]]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': null}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

// Descriptive test. The semantics for key generation are odd for positional key patterns.
TEST(BtreeKeyGeneratorTest, GetKeysPositionalKeyPatternNestedArray2) {
    BSONObj keyPattern = fromjson("{'a.2': 1}");
    BSONObj genKeysFrom = fromjson("{a: [[1, 2, 5], [3, 4, 6], [0, 1, 2]]}");
    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      fromjson("{'': [0, 1, 2]}"),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

// Descriptive test. The semantics for key generation are odd for positional key patterns.
TEST(BtreeKeyGeneratorTest, GetKeysPositionalKeyPatternNestedArray3) {
    BSONObj keyPattern = fromjson("{'a.2': 1}");
    BSONObj genKeysFrom = fromjson("{a: [{'0': 1, '1': 2, '2': 5}]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 5}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

// Descriptive test. The semantics for key generation are odd for positional key patterns.
TEST(BtreeKeyGeneratorTest, GetKeysPositionalKeyPatternNestedArray4) {
    BSONObj keyPattern = fromjson("{'a.b.2': 1}");
    BSONObj genKeysFrom = fromjson("{a: [{b: [[1, 2, 5]]}]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': null}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{0U, 1U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

// Descriptive test. The semantics for key generation are odd for positional key patterns.
TEST(BtreeKeyGeneratorTest, GetKeysPositionalKeyPatternNestedArray5) {
    BSONObj keyPattern = fromjson("{'a.2': 1}");
    BSONObj genKeysFrom = fromjson("{a: [[1, 2, 5], {'2': 6}]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': null}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 6}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetNullKeyNestedArray) {
    BSONObj keyPattern = fromjson("{'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: [[1, 2, 5]]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': null}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysUnevenNestedArrays) {
    BSONObj keyPattern = fromjson("{a: 1, 'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: [1, {b: [2, 3, 4]}]}");
    key_string::HeapBuilder keyString1(key_string::Version::kLatestVersion,
                                       fromjson("{'': 1, '': null}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(key_string::Version::kLatestVersion,
                                       fromjson("{'': {b:[2,3,4]}, '': 2}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(key_string::Version::kLatestVersion,
                                       fromjson("{'': {b:[2,3,4]}, '': 3}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString4(key_string::Version::kLatestVersion,
                                       fromjson("{'': {b:[2,3,4]}, '': 4}"),
                                       Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{
        keyString1.release(), keyString2.release(), keyString3.release(), keyString4.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}, {0U, 1U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

// Descriptive test. Should we define better semantics for future index versions in the case of
// repeated field names?
TEST(BtreeKeyGeneratorTest, GetKeysRepeatedFieldName) {
    BSONObj keyPattern = fromjson("{a: 1}");
    BSONObj genKeysFrom = fromjson("{a: 2, a: 3}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

// Descriptive test. Future index versions may want different or at least more consistent
// handling of empty path components.
TEST(BtreeKeyGeneratorTest, GetKeysEmptyPathPiece) {
    BSONObj keyPattern = fromjson("{'a..c': 1}");
    BSONObj genKeysFrom = fromjson("{a: {'': [{c: 1}, {c: 2}]}}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release()};
    MultikeyPaths expectedMultikeyPaths{{1U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

// Descriptive test. Future index versions may want different or at least more consistent
// handling of empty path components.
TEST(BtreeKeyGeneratorTest, GetKeysLastPathPieceEmpty) {
    BSONObj keyPattern = fromjson("{'a.': 1}");
    BSONObj genKeysFrom = fromjson("{a: 2}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': {'': 2}}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));

    genKeysFrom = fromjson("{a: {'': 2}}");
    expectedKeys.clear();
    expectedKeys.insert(keyString2.release());
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFirstPathPieceEmpty) {
    BSONObj keyPattern = fromjson("{'.a': 1}");
    BSONObj genKeysFrom = fromjson("{a: 2}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': null}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetKeysFirstPathPieceEmpty2) {
    BSONObj keyPattern = fromjson("{'.a': 1}");
    BSONObj genKeysFrom = fromjson("{'': [{a: [1, 2, 3]}]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(
        key_string::Version::kLatestVersion, fromjson("{'': 3}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};
    MultikeyPaths expectedMultikeyPaths{{0U, 1U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, PositionalKeyPatternParallelArrays) {
    BSONObj keyPattern = fromjson("{a: 1, 'b.0': 1}");
    BSONObj genKeysFrom = fromjson("{a: [1], b: [2]}");
    KeyStringSet expectedKeys;
    MultikeyPaths expectedMultikeyPaths(keyPattern.nFields());
    ASSERT_THROWS(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths),
                  AssertionException);
}

TEST(BtreeKeyGeneratorTest, KeyPattern_a_0_b_Extracts_b_ElementInsideSingleton2DArray) {
    BSONObj keyPattern = fromjson("{'a.0.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: [[{b: 1}]]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{1U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, KeyPattern_a_0_0_b_Extracts_b_ElementInsideSingleton2DArray) {
    BSONObj keyPattern = fromjson("{'a.0.0.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: [[{b: 1}]]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest,
     KeyPattern_a_0_0_b_ExtractsEachValueFrom_b_ArrayInsideSingleton2DArray) {
    BSONObj keyPattern = fromjson("{'a.0.0.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: [[{b: [1, 2, 3]}]]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(
        key_string::Version::kLatestVersion, fromjson("{'': 3}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};
    MultikeyPaths expectedMultikeyPaths{{3U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, KeyPattern_a_0_0_b_Extracts_b_ElementInsideSingleton3DArray) {
    BSONObj keyPattern = fromjson("{'a.0.0.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: [[[ {b: 1} ]]]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{2U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, KeyPattern_a_0_0_b_ExtractsEach_b_ElementInside3DArray) {
    BSONObj keyPattern = fromjson("{'a.0.0.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: [[[{b: 1}, {b: 2}, {b: 3}]]]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 1}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(
        key_string::Version::kLatestVersion, fromjson("{'': 3}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};
    MultikeyPaths expectedMultikeyPaths{{2U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, KeyPattern_a_0_0_b_ExtractsNullFrom4DArray) {
    BSONObj keyPattern = fromjson("{'a.0.0.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: [[[[ {b: 1} ]]]]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': null}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{2U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, PositionalKeyPatternNestedArrays5) {
    BSONObj keyPattern = fromjson("{'a.b.1': 1}");
    BSONObj genKeysFrom = fromjson("{a: [{b: [1, 2]}]}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 2}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

// Descriptive test.
TEST(BtreeKeyGeneratorTest, PositionalKeyPatternNestedArrays6) {
    BSONObj keyPattern = fromjson("{'a': 1, 'a.b': 1, 'a.0.b':1, 'a.b.0': 1, 'a.0.b.0': 1}");
    BSONObj genKeysFrom = fromjson("{a: [{b: [1,2]}, {b: 3}]}");
    key_string::HeapBuilder keyString1(key_string::Version::kLatestVersion,
                                       fromjson("{'': {b:3}, '': 3, '': 1, '': null, '': 1}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(key_string::Version::kLatestVersion,
                                       fromjson("{'': {b:3}, '': 3, '': 2, '': null, '': 1}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(key_string::Version::kLatestVersion,
                                       fromjson("{'': {b:[1,2]}, '': 1, '': 1, '': 1, '': 1}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString4(key_string::Version::kLatestVersion,
                                       fromjson("{'': {b:[1,2]}, '': 2, '': 2, '': 1, '': 1}"),
                                       Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{
        keyString1.release(), keyString2.release(), keyString3.release(), keyString4.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}, {0U, 1U}, {2U}, {0U}, MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

// Descriptive test.
TEST(BtreeKeyGeneratorTest, PositionalKeyPatternNestedArrays7) {
    BSONObj keyPattern = fromjson("{'a': 1, 'a.b': 1, 'a.0.b':1, 'a.b.0': 1, 'a.0.b.0': 1}");
    BSONObj genKeysFrom = fromjson("{a: [{b: [1,2]}, {b: {'0': 3}}]}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion,
        fromjson("{'': {b:{'0':3}}, '': {'0':3}, '': 1, '': 3, '': 1}"),
        Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion,
        fromjson("{'': {b:{'0':3}}, '': {'0':3}, '': 2, '': 3, '': 1}"),
        Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(key_string::Version::kLatestVersion,
                                       fromjson("{'': {b:[1,2]}, '': 1, '': 1, '': 1, '': 1}"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString4(key_string::Version::kLatestVersion,
                                       fromjson("{'': {b:[1,2]}, '': 2, '': 2, '': 1, '': 1}"),
                                       Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{
        keyString1.release(), keyString2.release(), keyString3.release(), keyString4.release()};
    MultikeyPaths expectedMultikeyPaths{{0U}, {0U, 1U}, {2U}, {0U}, MultikeyComponents{}};
    ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths));
}

TEST(BtreeKeyGeneratorTest, GetCollationAwareIdKeyFromObject) {
    BSONObj keyPattern = fromjson("{_id: 1}");
    BSONObj genKeysFrom = fromjson("{_id: 'foo', b: 4}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 'oof'}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(
        testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, false, &collator));
}

TEST(BtreeKeyGeneratorTest, GetCollationAwareKeysFromObjectSimple) {
    BSONObj keyPattern = fromjson("{a: 1}");
    BSONObj genKeysFrom = fromjson("{b: 4, a: 'foo'}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 'oof'}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(
        testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, false, &collator));
}

TEST(BtreeKeyGeneratorTest, GetCollationAwareKeysFromObjectDotted) {
    BSONObj keyPattern = fromjson("{'a.b': 1}");
    BSONObj genKeysFrom = fromjson("{a: {b: 'foo'}, c: 4}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 'oof'}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(
        testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, false, &collator));
}

TEST(BtreeKeyGeneratorTest, GetCollationAwareKeysFromArraySimple) {
    BSONObj keyPattern = fromjson("{a: 1}");
    BSONObj genKeysFrom = fromjson("{a: ['foo', 'bar', 'baz']}");
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, fromjson("{'': 'oof'}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, fromjson("{'': 'rab'}"), Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(
        key_string::Version::kLatestVersion, fromjson("{'': 'zab'}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    MultikeyPaths expectedMultikeyPaths{{0U}};
    ASSERT(
        testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, false, &collator));
}

TEST(BtreeKeyGeneratorTest, CollatorDoesNotAffectNonStringIdKey) {
    BSONObj keyPattern = fromjson("{_id: 1}");
    BSONObj genKeysFrom = fromjson("{_id: 5, b: 4}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 5}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(
        testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, false, &collator));
}

TEST(BtreeKeyGeneratorTest, CollatorDoesNotAffectNonStringKeys) {
    BSONObj keyPattern = fromjson("{a: 1}");
    BSONObj genKeysFrom = fromjson("{b: 4, a: 5}");
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, fromjson("{'': 5}"), Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(
        testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, false, &collator));
}

TEST(BtreeKeyGeneratorTest, GetCollationAwareKeysFromNestedObject) {
    BSONObj keyPattern = fromjson("{a: 1}");
    BSONObj genKeysFrom = fromjson("{b: 4, a: {c: 'foo'}}");
    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      fromjson("{'': {c: 'oof'}}"),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(
        testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, false, &collator));
}

TEST(BtreeKeyGeneratorTest, GetCollationAwareKeysFromNestedArray) {
    BSONObj keyPattern = fromjson("{a: 1}");
    BSONObj genKeysFrom = fromjson("{b: 4, a: {c: ['foo', 'bar', 'baz']}}");
    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      fromjson("{'': {c: ['oof', 'rab', 'zab']}}"),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    MultikeyPaths expectedMultikeyPaths{MultikeyComponents{}};
    ASSERT(
        testKeygen(keyPattern, genKeysFrom, expectedKeys, expectedMultikeyPaths, false, &collator));
}

}  // namespace
