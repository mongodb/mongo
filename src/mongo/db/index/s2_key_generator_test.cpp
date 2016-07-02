/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/index/expression_keys_private.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

using namespace mongo;

namespace {

std::string dumpKeyset(const BSONObjSet& objs) {
    std::stringstream ss;
    ss << "[ ";
    for (BSONObjSet::iterator i = objs.begin(); i != objs.end(); ++i) {
        ss << i->toString() << " ";
    }
    ss << "]";

    return ss.str();
}

std::string dumpMultikeyPaths(const MultikeyPaths& multikeyPaths) {
    std::stringstream ss;

    ss << "[ ";
    for (const auto multikeyComponents : multikeyPaths) {
        ss << "[ ";
        for (const auto multikeyComponent : multikeyComponents) {
            ss << multikeyComponent << " ";
        }
        ss << "] ";
    }
    ss << "]";

    return ss.str();
}

void assertKeysetsEqual(const BSONObjSet& expectedKeys, const BSONObjSet& actualKeys) {
    if (expectedKeys != actualKeys) {
        FAIL(str::stream() << "Expected: " << dumpKeyset(expectedKeys) << ", Actual: "
                           << dumpKeyset(actualKeys));
    }
}

void assertMultikeyPathsEqual(const MultikeyPaths& expectedMultikeyPaths,
                              const MultikeyPaths& actualMultikeyPaths) {
    if (expectedMultikeyPaths != actualMultikeyPaths) {
        FAIL(str::stream() << "Expected: " << dumpMultikeyPaths(expectedMultikeyPaths)
                           << ", Actual: "
                           << dumpMultikeyPaths(actualMultikeyPaths));
    }
}

long long getCellID(int x, int y, bool multiPoint = false) {
    BSONObj obj;
    if (multiPoint) {
        obj = BSON("a" << BSON("type"
                               << "MultiPoint"
                               << "coordinates"
                               << BSON_ARRAY(BSON_ARRAY(x << y))));
    } else {
        obj = BSON("a" << BSON("type"
                               << "Point"
                               << "coordinates"
                               << BSON_ARRAY(x << y)));
    }
    BSONObj keyPattern = fromjson("{a: '2dsphere'}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    const CollatorInterface* collator = nullptr;
    ExpressionParams::initialize2dsphereParams(infoObj, collator, &params);

    BSONObjSet keys;
    // There's no need to compute the prefixes of the indexed fields that cause the index to be
    // multikey when computing the cell id of the geo field.
    MultikeyPaths* multikeyPaths = nullptr;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &keys, multikeyPaths);

    ASSERT_EQUALS(1U, keys.size());
    return (*keys.begin()).firstElement().Long();
}

TEST(S2KeyGeneratorTest, GetS2KeysFromSubobjectWithArrayOfGeoAndNonGeoSubobjects) {
    BSONObj keyPattern = fromjson("{'a.b.nongeo': 1, 'a.b.geo': '2dsphere'}");
    BSONObj genKeysFrom = fromjson(
        "{a: {b: [{nongeo: 1, geo: {type: 'Point', coordinates: [0, 0]}}, "
        "{nongeo: 2, geo: {type: 'Point', coordinates: [3, 3]}}]}}");
    BSONObj infoObj =
        fromjson("{key: {'a.b.nongeo': 1, 'a.b.geo': '2dsphere'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock* collator = nullptr;
    ExpressionParams::initialize2dsphereParams(infoObj, collator, &params);

    BSONObjSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    ExpressionKeysPrivate::getS2Keys(
        genKeysFrom, keyPattern, params, &actualKeys, &actualMultikeyPaths);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON("" << 1 << "" << getCellID(0, 0)));
    expectedKeys.insert(BSON("" << 1 << "" << getCellID(3, 3)));
    expectedKeys.insert(BSON("" << 2 << "" << getCellID(0, 0)));
    expectedKeys.insert(BSON("" << 2 << "" << getCellID(3, 3)));

    assertKeysetsEqual(expectedKeys, actualKeys);
    assertMultikeyPathsEqual(MultikeyPaths{{1U}, {1U}}, actualMultikeyPaths);
}

TEST(S2KeyGeneratorTest, GetS2KeysFromArrayOfNonGeoSubobjectsWithArrayValues) {
    BSONObj keyPattern = fromjson("{'a.nongeo': 1, geo: '2dsphere'}");
    BSONObj genKeysFrom = fromjson(
        "{a: [{nongeo: [1, 2]}, {nongeo: [2, 3]}], "
        "geo: {type: 'Point', coordinates: [0, 0]}}");
    BSONObj infoObj =
        fromjson("{key: {'a.nongeo': 1, geo: '2dsphere'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock* collator = nullptr;
    ExpressionParams::initialize2dsphereParams(infoObj, collator, &params);

    BSONObjSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    ExpressionKeysPrivate::getS2Keys(
        genKeysFrom, keyPattern, params, &actualKeys, &actualMultikeyPaths);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON("" << 1 << "" << getCellID(0, 0)));
    expectedKeys.insert(BSON("" << 2 << "" << getCellID(0, 0)));
    expectedKeys.insert(BSON("" << 3 << "" << getCellID(0, 0)));

    assertKeysetsEqual(expectedKeys, actualKeys);
    assertMultikeyPathsEqual(MultikeyPaths{{0U, 1U}, std::set<size_t>{}}, actualMultikeyPaths);
}

TEST(S2KeyGeneratorTest, GetS2KeysFromMultiPointInGeoField) {
    BSONObj keyPattern = fromjson("{nongeo: 1, geo: '2dsphere'}");
    BSONObj genKeysFrom =
        fromjson("{nongeo: 1, geo: {type: 'MultiPoint', coordinates: [[0, 0], [1, 0], [1, 1]]}}");
    BSONObj infoObj = fromjson("{key: {nongeo: 1, geo: '2dsphere'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock* collator = nullptr;
    ExpressionParams::initialize2dsphereParams(infoObj, collator, &params);

    BSONObjSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    ExpressionKeysPrivate::getS2Keys(
        genKeysFrom, keyPattern, params, &actualKeys, &actualMultikeyPaths);

    const bool multiPoint = true;
    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON("" << 1 << "" << getCellID(0, 0, multiPoint)));
    expectedKeys.insert(BSON("" << 1 << "" << getCellID(1, 0, multiPoint)));
    expectedKeys.insert(BSON("" << 1 << "" << getCellID(1, 1, multiPoint)));

    assertKeysetsEqual(expectedKeys, actualKeys);
    assertMultikeyPathsEqual(MultikeyPaths{std::set<size_t>{}, {0U}}, actualMultikeyPaths);
}

TEST(S2KeyGeneratorTest, CollationAppliedToNonGeoStringFieldAfterGeoField) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: 'string'}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionParams::initialize2dsphereParams(infoObj, &collator, &params);

    BSONObjSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys, &actualMultikeyPaths);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON("" << getCellID(0, 0) << ""
                                << "gnirts"));

    assertKeysetsEqual(expectedKeys, actualKeys);
    assertMultikeyPathsEqual(MultikeyPaths{std::set<size_t>{}, std::set<size_t>{}},
                             actualMultikeyPaths);
}

TEST(S2KeyGeneratorTest, CollationAppliedToNonGeoStringFieldBeforeGeoField) {
    BSONObj obj = fromjson("{a: 'string', b: {type: 'Point', coordinates: [0, 0]}}");
    BSONObj keyPattern = fromjson("{a: 1, b: '2dsphere'}");
    BSONObj infoObj = fromjson("{key: {a: 1, b: '2dsphere'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionParams::initialize2dsphereParams(infoObj, &collator, &params);

    BSONObjSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys, &actualMultikeyPaths);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON(""
                             << "gnirts"
                             << ""
                             << getCellID(0, 0)));

    assertKeysetsEqual(expectedKeys, actualKeys);
    assertMultikeyPathsEqual(MultikeyPaths{std::set<size_t>{}, std::set<size_t>{}},
                             actualMultikeyPaths);
}

TEST(S2KeyGeneratorTest, CollationAppliedToAllNonGeoStringFields) {
    BSONObj obj = fromjson("{a: 'string', b: {type: 'Point', coordinates: [0, 0]}, c: 'string2'}");
    BSONObj keyPattern = fromjson("{a: 1, b: '2dsphere', c: 1}");
    BSONObj infoObj = fromjson("{key: {a: 1, b: '2dsphere', c: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionParams::initialize2dsphereParams(infoObj, &collator, &params);

    BSONObjSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys, &actualMultikeyPaths);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON(""
                             << "gnirts"
                             << ""
                             << getCellID(0, 0)
                             << ""
                             << "2gnirts"));

    assertKeysetsEqual(expectedKeys, actualKeys);
    assertMultikeyPathsEqual(
        MultikeyPaths{std::set<size_t>{}, std::set<size_t>{}, std::set<size_t>{}},
        actualMultikeyPaths);
}

TEST(S2KeyGeneratorTest, CollationAppliedToNonGeoStringFieldWithMultiplePathComponents) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: {c: {d: 'string'}}}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', 'b.c.d': 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', 'b.c.d': 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionParams::initialize2dsphereParams(infoObj, &collator, &params);

    BSONObjSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys, &actualMultikeyPaths);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON("" << getCellID(0, 0) << ""
                                << "gnirts"));

    assertKeysetsEqual(expectedKeys, actualKeys);
    assertMultikeyPathsEqual(MultikeyPaths{std::set<size_t>{}, std::set<size_t>{}},
                             actualMultikeyPaths);
}

TEST(S2KeyGeneratorTest, CollationAppliedToStringsInArray) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: ['string', 'string2']}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionParams::initialize2dsphereParams(infoObj, &collator, &params);

    BSONObjSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys, &actualMultikeyPaths);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON("" << getCellID(0, 0) << ""
                                << "gnirts"));
    expectedKeys.insert(BSON("" << getCellID(0, 0) << ""
                                << "2gnirts"));

    assertKeysetsEqual(expectedKeys, actualKeys);
    assertMultikeyPathsEqual(MultikeyPaths{std::set<size_t>{}, {0U}}, actualMultikeyPaths);
}

TEST(S2KeyGeneratorTest, CollationAppliedToStringsInAllArrays) {
    BSONObj obj = fromjson(
        "{a: {type: 'Point', coordinates: [0, 0]}, b: ['string', 'string2'], c: ['abc', 'def']}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1, c: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1, c: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionParams::initialize2dsphereParams(infoObj, &collator, &params);

    BSONObjSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys, &actualMultikeyPaths);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON("" << getCellID(0, 0) << ""
                                << "gnirts"
                                << ""
                                << "cba"));
    expectedKeys.insert(BSON("" << getCellID(0, 0) << ""
                                << "gnirts"
                                << ""
                                << "fed"));
    expectedKeys.insert(BSON("" << getCellID(0, 0) << ""
                                << "2gnirts"
                                << ""
                                << "cba"));
    expectedKeys.insert(BSON("" << getCellID(0, 0) << ""
                                << "2gnirts"
                                << ""
                                << "fed"));

    assertKeysetsEqual(expectedKeys, actualKeys);
    assertMultikeyPathsEqual(MultikeyPaths{std::set<size_t>{}, {0U}, {0U}}, actualMultikeyPaths);
}

TEST(S2KeyGeneratorTest, CollationDoesNotAffectNonStringFields) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: 5}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionParams::initialize2dsphereParams(infoObj, &collator, &params);

    BSONObjSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys, &actualMultikeyPaths);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON("" << getCellID(0, 0) << "" << 5));

    assertKeysetsEqual(expectedKeys, actualKeys);
    assertMultikeyPathsEqual(MultikeyPaths{std::set<size_t>{}, std::set<size_t>{}},
                             actualMultikeyPaths);
}

TEST(S2KeyGeneratorTest, CollationAppliedToStringsInNestedObjects) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: {c: 'string'}}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionParams::initialize2dsphereParams(infoObj, &collator, &params);

    BSONObjSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys, &actualMultikeyPaths);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON("" << getCellID(0, 0) << "" << BSON("c"
                                                                 << "gnirts")));

    assertKeysetsEqual(expectedKeys, actualKeys);
    assertMultikeyPathsEqual(MultikeyPaths{std::set<size_t>{}, std::set<size_t>{}},
                             actualMultikeyPaths);
}

TEST(S2KeyGeneratorTest, NoCollation) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: 'string'}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    const CollatorInterface* collator = nullptr;
    ExpressionParams::initialize2dsphereParams(infoObj, collator, &params);

    BSONObjSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys, &actualMultikeyPaths);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON("" << getCellID(0, 0) << ""
                                << "string"));

    assertKeysetsEqual(expectedKeys, actualKeys);
    assertMultikeyPathsEqual(MultikeyPaths{std::set<size_t>{}, std::set<size_t>{}},
                             actualMultikeyPaths);
}

}  // namespace
