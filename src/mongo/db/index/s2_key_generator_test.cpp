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

#include "mongo/db/index/s2_key_generator.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/shared_buffer_fragment.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <ostream>
#include <string>

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <util/math/mathutil.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


using namespace mongo;

namespace {

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

bool areKeysetsEqual(const KeyStringSet& expectedKeys, const KeyStringSet& actualKeys) {
    if (expectedKeys.size() != actualKeys.size()) {
        LOGV2(20693,
              "Expected: {dumpKeyset_expectedKeys}, Actual: {dumpKeyset_actualKeys}",
              "dumpKeyset_expectedKeys"_attr = dumpKeyset(expectedKeys),
              "dumpKeyset_actualKeys"_attr = dumpKeyset(actualKeys));
        return false;
    }

    if (!std::equal(expectedKeys.begin(), expectedKeys.end(), actualKeys.begin())) {
        LOGV2(20694,
              "Expected: {dumpKeyset_expectedKeys}, Actual: {dumpKeyset_actualKeys}",
              "dumpKeyset_expectedKeys"_attr = dumpKeyset(expectedKeys),
              "dumpKeyset_actualKeys"_attr = dumpKeyset(actualKeys));
        return false;
    }

    return true;
}

void assertMultikeyPathsEqual(const MultikeyPaths& expectedMultikeyPaths,
                              const MultikeyPaths& actualMultikeyPaths) {
    if (expectedMultikeyPaths != actualMultikeyPaths) {
        FAIL(std::string(str::stream() << "Expected: " << dumpMultikeyPaths(expectedMultikeyPaths)
                                       << ", Actual: " << dumpMultikeyPaths(actualMultikeyPaths)));
    }
}

struct S2KeyGeneratorTest : public unittest::Test {
    SharedBufferFragmentBuilder allocator{key_string::HeapBuilder::kHeapAllocatorDefaultBytes};

    long long getCellID(int x, int y, bool multiPoint = false) {
        BSONObj obj;
        if (multiPoint) {
            obj = BSON("a" << BSON("type" << "MultiPoint"
                                          << "coordinates" << BSON_ARRAY(BSON_ARRAY(x << y))));
        } else {
            obj = BSON("a" << BSON("type" << "Point"
                                          << "coordinates" << BSON_ARRAY(x << y)));
        }
        BSONObj keyPattern = fromjson("{a: '2dsphere'}");
        BSONObj infoObj = fromjson("{key: {a: '2dsphere'}, '2dsphereIndexVersion': 3}");
        S2IndexingParams params;
        const CollatorInterface* collator = nullptr;
        index2dsphere::initialize2dsphereParams(infoObj, collator, &params);

        KeyStringSet keys;
        // There's no need to compute the prefixes of the indexed fields that cause the index to be
        // multikey when computing the cell id of the geo field.
        MultikeyPaths* multikeyPaths = nullptr;
        index2dsphere::getS2Keys(allocator,
                                 obj,
                                 keyPattern,
                                 params,
                                 &keys,
                                 multikeyPaths,
                                 key_string::Version::kLatestVersion,
                                 SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                                 Ordering::make(BSONObj()));

        ASSERT_EQUALS(1U, keys.size());
        auto key = key_string::toBson(*keys.begin(), Ordering::make(BSONObj()));
        return key.firstElement().Long();
    }
};

TEST_F(S2KeyGeneratorTest, GetS2KeysFromSubobjectWithArrayOfGeoAndNonGeoSubobjects) {
    BSONObj keyPattern = fromjson("{'a.b.nongeo': 1, 'a.b.geo': '2dsphere'}");
    BSONObj genKeysFrom = fromjson(
        "{a: {b: [{nongeo: 1, geo: {type: 'Point', coordinates: [0, 0]}}, "
        "{nongeo: 2, geo: {type: 'Point', coordinates: [3, 3]}}]}}");
    BSONObj infoObj =
        fromjson("{key: {'a.b.nongeo': 1, 'a.b.geo': '2dsphere'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock* collator = nullptr;
    index2dsphere::initialize2dsphereParams(infoObj, collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    index2dsphere::getS2Keys(allocator,
                             genKeysFrom,
                             keyPattern,
                             params,
                             &actualKeys,
                             &actualMultikeyPaths,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()));

    key_string::HeapBuilder keyString1(key_string::Version::kLatestVersion,
                                       BSON("" << 1 << "" << getCellID(0, 0)),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(key_string::Version::kLatestVersion,
                                       BSON("" << 1 << "" << getCellID(3, 3)),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(key_string::Version::kLatestVersion,
                                       BSON("" << 2 << "" << getCellID(0, 0)),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString4(key_string::Version::kLatestVersion,
                                       BSON("" << 2 << "" << getCellID(3, 3)),
                                       Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{
        keyString1.release(), keyString2.release(), keyString3.release(), keyString4.release()};

    ASSERT_TRUE(areKeysetsEqual(expectedKeys, actualKeys));
    assertMultikeyPathsEqual(MultikeyPaths{{1U}, {1U}}, actualMultikeyPaths);
}

TEST_F(S2KeyGeneratorTest, GetS2KeysFromArrayOfNonGeoSubobjectsWithArrayValues) {
    BSONObj keyPattern = fromjson("{'a.nongeo': 1, geo: '2dsphere'}");
    BSONObj genKeysFrom = fromjson(
        "{a: [{nongeo: [1, 2]}, {nongeo: [2, 3]}], "
        "geo: {type: 'Point', coordinates: [0, 0]}}");
    BSONObj infoObj =
        fromjson("{key: {'a.nongeo': 1, geo: '2dsphere'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock* collator = nullptr;
    index2dsphere::initialize2dsphereParams(infoObj, collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    index2dsphere::getS2Keys(allocator,
                             genKeysFrom,
                             keyPattern,
                             params,
                             &actualKeys,
                             &actualMultikeyPaths,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()));

    key_string::HeapBuilder keyString1(key_string::Version::kLatestVersion,
                                       BSON("" << 1 << "" << getCellID(0, 0)),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(key_string::Version::kLatestVersion,
                                       BSON("" << 2 << "" << getCellID(0, 0)),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(key_string::Version::kLatestVersion,
                                       BSON("" << 3 << "" << getCellID(0, 0)),
                                       Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};

    ASSERT_TRUE(areKeysetsEqual(expectedKeys, actualKeys));
    assertMultikeyPathsEqual(MultikeyPaths{{0U, 1U}, MultikeyComponents{}}, actualMultikeyPaths);
}

TEST_F(S2KeyGeneratorTest, GetS2KeysFromMultiPointInGeoField) {
    BSONObj keyPattern = fromjson("{nongeo: 1, geo: '2dsphere'}");
    BSONObj genKeysFrom =
        fromjson("{nongeo: 1, geo: {type: 'MultiPoint', coordinates: [[0, 0], [1, 0], [1, 1]]}}");
    BSONObj infoObj = fromjson("{key: {nongeo: 1, geo: '2dsphere'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock* collator = nullptr;
    index2dsphere::initialize2dsphereParams(infoObj, collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    index2dsphere::getS2Keys(allocator,
                             genKeysFrom,
                             keyPattern,
                             params,
                             &actualKeys,
                             &actualMultikeyPaths,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()));

    const bool multiPoint = true;
    key_string::HeapBuilder keyString1(key_string::Version::kLatestVersion,
                                       BSON("" << 1 << "" << getCellID(0, 0, multiPoint)),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(key_string::Version::kLatestVersion,
                                       BSON("" << 1 << "" << getCellID(1, 0, multiPoint)),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(key_string::Version::kLatestVersion,
                                       BSON("" << 1 << "" << getCellID(1, 1, multiPoint)),
                                       Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release(), keyString3.release()};

    ASSERT_TRUE(areKeysetsEqual(expectedKeys, actualKeys));
    assertMultikeyPathsEqual(MultikeyPaths{MultikeyComponents{}, {0U}}, actualMultikeyPaths);
}

TEST_F(S2KeyGeneratorTest, CollationAppliedToNonGeoStringFieldAfterGeoField) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: 'string'}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    index2dsphere::initialize2dsphereParams(infoObj, &collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    index2dsphere::getS2Keys(allocator,
                             obj,
                             keyPattern,
                             params,
                             &actualKeys,
                             &actualMultikeyPaths,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()));

    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      BSON("" << getCellID(0, 0) << ""
                                              << "gnirts"),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};

    ASSERT_TRUE(areKeysetsEqual(expectedKeys, actualKeys));
    assertMultikeyPathsEqual(MultikeyPaths{MultikeyComponents{}, MultikeyComponents{}},
                             actualMultikeyPaths);
}

TEST_F(S2KeyGeneratorTest, CollationAppliedToNonGeoStringFieldBeforeGeoField) {
    BSONObj obj = fromjson("{a: 'string', b: {type: 'Point', coordinates: [0, 0]}}");
    BSONObj keyPattern = fromjson("{a: 1, b: '2dsphere'}");
    BSONObj infoObj = fromjson("{key: {a: 1, b: '2dsphere'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    index2dsphere::initialize2dsphereParams(infoObj, &collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    index2dsphere::getS2Keys(allocator,
                             obj,
                             keyPattern,
                             params,
                             &actualKeys,
                             &actualMultikeyPaths,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()));

    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      BSON("" << "gnirts"
                                              << "" << getCellID(0, 0)),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};

    ASSERT_TRUE(areKeysetsEqual(expectedKeys, actualKeys));
    assertMultikeyPathsEqual(MultikeyPaths{MultikeyComponents{}, MultikeyComponents{}},
                             actualMultikeyPaths);
}

TEST_F(S2KeyGeneratorTest, CollationAppliedToAllNonGeoStringFields) {
    BSONObj obj = fromjson("{a: 'string', b: {type: 'Point', coordinates: [0, 0]}, c: 'string2'}");
    BSONObj keyPattern = fromjson("{a: 1, b: '2dsphere', c: 1}");
    BSONObj infoObj = fromjson("{key: {a: 1, b: '2dsphere', c: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    index2dsphere::initialize2dsphereParams(infoObj, &collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    index2dsphere::getS2Keys(allocator,
                             obj,
                             keyPattern,
                             params,
                             &actualKeys,
                             &actualMultikeyPaths,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()));

    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      BSON("" << "gnirts"
                                              << "" << getCellID(0, 0) << ""
                                              << "2gnirts"),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};

    ASSERT_TRUE(areKeysetsEqual(expectedKeys, actualKeys));
    assertMultikeyPathsEqual(
        MultikeyPaths{MultikeyComponents{}, MultikeyComponents{}, MultikeyComponents{}},
        actualMultikeyPaths);
}

TEST_F(S2KeyGeneratorTest, CollationAppliedToNonGeoStringFieldWithMultiplePathComponents) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: {c: {d: 'string'}}}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', 'b.c.d': 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', 'b.c.d': 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    index2dsphere::initialize2dsphereParams(infoObj, &collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    index2dsphere::getS2Keys(allocator,
                             obj,
                             keyPattern,
                             params,
                             &actualKeys,
                             &actualMultikeyPaths,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()));

    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      BSON("" << getCellID(0, 0) << ""
                                              << "gnirts"),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};

    ASSERT_TRUE(areKeysetsEqual(expectedKeys, actualKeys));
    assertMultikeyPathsEqual(MultikeyPaths{MultikeyComponents{}, MultikeyComponents{}},
                             actualMultikeyPaths);
}

TEST_F(S2KeyGeneratorTest, CollationAppliedToStringsInArray) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: ['string', 'string2']}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    index2dsphere::initialize2dsphereParams(infoObj, &collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    index2dsphere::getS2Keys(allocator,
                             obj,
                             keyPattern,
                             params,
                             &actualKeys,
                             &actualMultikeyPaths,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()));

    key_string::HeapBuilder keyString1(key_string::Version::kLatestVersion,
                                       BSON("" << getCellID(0, 0) << ""
                                               << "gnirts"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(key_string::Version::kLatestVersion,
                                       BSON("" << getCellID(0, 0) << ""
                                               << "2gnirts"),
                                       Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString1.release(), keyString2.release()};

    ASSERT_TRUE(areKeysetsEqual(expectedKeys, actualKeys));
    assertMultikeyPathsEqual(MultikeyPaths{MultikeyComponents{}, {0U}}, actualMultikeyPaths);
}

TEST_F(S2KeyGeneratorTest, CollationAppliedToStringsInAllArrays) {
    BSONObj obj = fromjson(
        "{a: {type: 'Point', coordinates: [0, 0]}, b: ['string', 'string2'], c: ['abc', 'def']}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1, c: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1, c: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    index2dsphere::initialize2dsphereParams(infoObj, &collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    index2dsphere::getS2Keys(allocator,
                             obj,
                             keyPattern,
                             params,
                             &actualKeys,
                             &actualMultikeyPaths,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()));

    key_string::HeapBuilder keyString1(key_string::Version::kLatestVersion,
                                       BSON("" << getCellID(0, 0) << ""
                                               << "gnirts"
                                               << ""
                                               << "cba"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString2(key_string::Version::kLatestVersion,
                                       BSON("" << getCellID(0, 0) << ""
                                               << "gnirts"
                                               << ""
                                               << "fed"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString3(key_string::Version::kLatestVersion,
                                       BSON("" << getCellID(0, 0) << ""
                                               << "2gnirts"
                                               << ""
                                               << "cba"),
                                       Ordering::make(BSONObj()));
    key_string::HeapBuilder keyString4(key_string::Version::kLatestVersion,
                                       BSON("" << getCellID(0, 0) << ""
                                               << "2gnirts"
                                               << ""
                                               << "fed"),
                                       Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{
        keyString1.release(), keyString2.release(), keyString3.release(), keyString4.release()};

    ASSERT_TRUE(areKeysetsEqual(expectedKeys, actualKeys));
    assertMultikeyPathsEqual(MultikeyPaths{MultikeyComponents{}, {0U}, {0U}}, actualMultikeyPaths);
}

TEST_F(S2KeyGeneratorTest, CollationDoesNotAffectNonStringFields) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: 5}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    index2dsphere::initialize2dsphereParams(infoObj, &collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    index2dsphere::getS2Keys(allocator,
                             obj,
                             keyPattern,
                             params,
                             &actualKeys,
                             &actualMultikeyPaths,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()));

    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      BSON("" << getCellID(0, 0) << "" << 5),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};

    ASSERT_TRUE(areKeysetsEqual(expectedKeys, actualKeys));
    assertMultikeyPathsEqual(MultikeyPaths{MultikeyComponents{}, MultikeyComponents{}},
                             actualMultikeyPaths);
}

TEST_F(S2KeyGeneratorTest, CollationAppliedToStringsInNestedObjects) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: {c: 'string'}}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    index2dsphere::initialize2dsphereParams(infoObj, &collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    index2dsphere::getS2Keys(allocator,
                             obj,
                             keyPattern,
                             params,
                             &actualKeys,
                             &actualMultikeyPaths,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()));

    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      BSON("" << getCellID(0, 0) << "" << BSON("c" << "gnirts")),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};

    ASSERT_TRUE(areKeysetsEqual(expectedKeys, actualKeys));
    assertMultikeyPathsEqual(MultikeyPaths{MultikeyComponents{}, MultikeyComponents{}},
                             actualMultikeyPaths);
}

TEST_F(S2KeyGeneratorTest, NoCollation) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: 'string'}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    const CollatorInterface* collator = nullptr;
    index2dsphere::initialize2dsphereParams(infoObj, collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    index2dsphere::getS2Keys(allocator,
                             obj,
                             keyPattern,
                             params,
                             &actualKeys,
                             &actualMultikeyPaths,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()));

    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      BSON("" << getCellID(0, 0) << ""
                                              << "string"),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};

    ASSERT_TRUE(areKeysetsEqual(expectedKeys, actualKeys));
    assertMultikeyPathsEqual(MultikeyPaths{MultikeyComponents{}, MultikeyComponents{}},
                             actualMultikeyPaths);
}

TEST_F(S2KeyGeneratorTest, EmptyArrayForLeadingFieldIsConsideredMultikey) {
    BSONObj obj = fromjson("{a: [], b: {type: 'Point', coordinates: [0, 0]}}");
    BSONObj keyPattern = fromjson("{a: 1, b: '2dsphere'}");
    BSONObj infoObj = fromjson("{key: {a: 1, b: '2dsphere'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    const CollatorInterface* collator = nullptr;
    index2dsphere::initialize2dsphereParams(infoObj, collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    index2dsphere::getS2Keys(allocator,
                             obj,
                             keyPattern,
                             params,
                             &actualKeys,
                             &actualMultikeyPaths,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()));

    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      BSON("" << BSONUndefined << "" << getCellID(0, 0)),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};

    ASSERT_TRUE(areKeysetsEqual(expectedKeys, actualKeys));
    assertMultikeyPathsEqual(MultikeyPaths{{0U}, MultikeyComponents{}}, actualMultikeyPaths);
}

TEST_F(S2KeyGeneratorTest, EmptyArrayForTrailingFieldIsConsideredMultikey) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: []}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', a: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    const CollatorInterface* collator = nullptr;
    index2dsphere::initialize2dsphereParams(infoObj, collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    index2dsphere::getS2Keys(allocator,
                             obj,
                             keyPattern,
                             params,
                             &actualKeys,
                             &actualMultikeyPaths,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()));

    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      BSON("" << getCellID(0, 0) << "" << BSONUndefined),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};

    ASSERT_TRUE(areKeysetsEqual(expectedKeys, actualKeys));
    assertMultikeyPathsEqual(MultikeyPaths{MultikeyComponents{}, {0U}}, actualMultikeyPaths);
}

TEST_F(S2KeyGeneratorTest, SingleElementTrailingArrayIsConsideredMultikey) {
    BSONObj obj = fromjson("{a: {c: [99]}, b: {type: 'Point', coordinates: [0, 0]}}");
    BSONObj keyPattern = fromjson("{'a.c': 1, b: '2dsphere'}");
    BSONObj infoObj = fromjson("{key: {'a.c': 1, b: '2dsphere'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    const CollatorInterface* collator = nullptr;
    index2dsphere::initialize2dsphereParams(infoObj, collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    index2dsphere::getS2Keys(allocator,
                             obj,
                             keyPattern,
                             params,
                             &actualKeys,
                             &actualMultikeyPaths,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()));

    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      BSON("" << 99 << "" << getCellID(0, 0)),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};

    ASSERT_TRUE(areKeysetsEqual(expectedKeys, actualKeys));
    assertMultikeyPathsEqual(MultikeyPaths{{1U}, MultikeyComponents{}}, actualMultikeyPaths);
}

TEST_F(S2KeyGeneratorTest, MidPathSingleElementArrayIsConsideredMultikey) {
    BSONObj obj = fromjson("{a: [{c: 99}], b: {type: 'Point', coordinates: [0, 0]}}");
    BSONObj keyPattern = fromjson("{'a.c': 1, b: '2dsphere'}");
    BSONObj infoObj = fromjson("{key: {'a.c': 1, b: '2dsphere'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    const CollatorInterface* collator = nullptr;
    index2dsphere::initialize2dsphereParams(infoObj, collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    index2dsphere::getS2Keys(allocator,
                             obj,
                             keyPattern,
                             params,
                             &actualKeys,
                             &actualMultikeyPaths,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()));

    key_string::HeapBuilder keyString(key_string::Version::kLatestVersion,
                                      BSON("" << 99 << "" << getCellID(0, 0)),
                                      Ordering::make(BSONObj()));
    KeyStringSet expectedKeys{keyString.release()};

    ASSERT_TRUE(areKeysetsEqual(expectedKeys, actualKeys));
    assertMultikeyPathsEqual(MultikeyPaths{{0U}, MultikeyComponents{}}, actualMultikeyPaths);
}

// Test which verifies that the rounding functions used by s2 follow 'round to even' rounding
// behavior.
TEST_F(S2KeyGeneratorTest, VerifyS2RoundingBehavior) {
    const double roundDownToEven = 2.5;
    ASSERT_EQ(2, MathUtil::FastIntRound(roundDownToEven));
    ASSERT_EQ(2LL, MathUtil::FastInt64Round(roundDownToEven));

    const double roundUpToEven = 3.5;
    ASSERT_EQ(4, MathUtil::FastIntRound(roundUpToEven));
    ASSERT_EQ(4LL, MathUtil::FastInt64Round(roundUpToEven));

    const double roundDownToEvenNegative = -3.5;
    ASSERT_EQ(-4, MathUtil::FastIntRound(roundDownToEvenNegative));
    ASSERT_EQ(-4LL, MathUtil::FastInt64Round(roundDownToEvenNegative));

    const double roundUpToEvenNegative = -2.5;
    ASSERT_EQ(-2, MathUtil::FastIntRound(roundUpToEvenNegative));
    ASSERT_EQ(-2LL, MathUtil::FastInt64Round(roundUpToEvenNegative));

    const double point = 944920918.5;
    ASSERT_EQ(944920918, MathUtil::FastIntRound(point));
    ASSERT_EQ(944920918LL, MathUtil::FastInt64Round(point));
}
}  // namespace
