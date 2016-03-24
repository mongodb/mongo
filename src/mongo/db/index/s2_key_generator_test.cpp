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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/index/expression_keys_private.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

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

bool assertKeysetsEqual(const BSONObjSet& expectedKeys, const BSONObjSet& actualKeys) {
    if (expectedKeys != actualKeys) {
        log() << "Expected: " << dumpKeyset(expectedKeys) << ", "
              << "Actual: " << dumpKeyset(actualKeys);
        return false;
    }
    return true;
}

long long getCellID(int x, int y) {
    BSONObj obj = BSON("a" << BSON("type"
                                   << "Point"
                                   << "coordinates" << BSON_ARRAY(x << y)));
    BSONObj keyPattern = fromjson("{a: '2dsphere'}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterface* collator = nullptr;
    ExpressionParams::initialize2dsphereParams(infoObj, collator, &params);
    BSONObjSet keys;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &keys);
    ASSERT_EQUALS(1U, keys.size());
    return (*keys.begin()).firstElement().Long();
}

TEST(S2KeyGeneratorTest, CollationAppliedToNonGeoStringFieldAfterGeoField) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: 'string'}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionParams::initialize2dsphereParams(infoObj, &collator, &params);
    BSONObjSet actualKeys;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON("" << getCellID(0, 0) << ""
                                << "gnirts"));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(S2KeyGeneratorTest, CollationAppliedToNonGeoStringFieldBeforeGeoField) {
    BSONObj obj = fromjson("{a: 'string', b: {type: 'Point', coordinates: [0, 0]}}");
    BSONObj keyPattern = fromjson("{a: 1, b: '2dsphere'}");
    BSONObj infoObj = fromjson("{key: {a: 1, b: '2dsphere'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionParams::initialize2dsphereParams(infoObj, &collator, &params);
    BSONObjSet actualKeys;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON(""
                             << "gnirts"
                             << "" << getCellID(0, 0)));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(S2KeyGeneratorTest, CollationAppliedToAllNonGeoStringFields) {
    BSONObj obj = fromjson("{a: 'string', b: {type: 'Point', coordinates: [0, 0]}, c: 'string2'}");
    BSONObj keyPattern = fromjson("{a: 1, b: '2dsphere', c: 1}");
    BSONObj infoObj = fromjson("{key: {a: 1, b: '2dsphere', c: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionParams::initialize2dsphereParams(infoObj, &collator, &params);
    BSONObjSet actualKeys;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON(""
                             << "gnirts"
                             << "" << getCellID(0, 0) << ""
                             << "2gnirts"));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(S2KeyGeneratorTest, CollationAppliedToNonGeoStringFieldWithMultiplePathComponents) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: {c: {d: 'string'}}}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', 'b.c.d': 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', 'b.c.d': 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionParams::initialize2dsphereParams(infoObj, &collator, &params);
    BSONObjSet actualKeys;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON("" << getCellID(0, 0) << ""
                                << "gnirts"));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(S2KeyGeneratorTest, CollationAppliedToStringsInArray) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: ['string', 'string2']}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionParams::initialize2dsphereParams(infoObj, &collator, &params);
    BSONObjSet actualKeys;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON("" << getCellID(0, 0) << ""
                                << "gnirts"));
    expectedKeys.insert(BSON("" << getCellID(0, 0) << ""
                                << "2gnirts"));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
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
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys);

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

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(S2KeyGeneratorTest, CollationDoesNotAffectNonStringFields) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: 5}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionParams::initialize2dsphereParams(infoObj, &collator, &params);
    BSONObjSet actualKeys;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON("" << getCellID(0, 0) << "" << 5));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

// TODO SERVER-23172: remove test.
TEST(S2KeyGeneratorTest, CollationDoesNotAffectStringsInEmbeddedDocuments) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: {c: 'string'}}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    ExpressionParams::initialize2dsphereParams(infoObj, &collator, &params);
    BSONObjSet actualKeys;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON("" << getCellID(0, 0) << "" << BSON("c"
                                                                 << "string")));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST(S2KeyGeneratorTest, NoCollation) {
    BSONObj obj = fromjson("{a: {type: 'Point', coordinates: [0, 0]}, b: 'string'}");
    BSONObj keyPattern = fromjson("{a: '2dsphere', b: 1}");
    BSONObj infoObj = fromjson("{key: {a: '2dsphere', b: 1}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterface* collator = nullptr;
    ExpressionParams::initialize2dsphereParams(infoObj, collator, &params);
    BSONObjSet actualKeys;
    ExpressionKeysPrivate::getS2Keys(obj, keyPattern, params, &actualKeys);

    BSONObjSet expectedKeys;
    expectedKeys.insert(BSON("" << getCellID(0, 0) << ""
                                << "string"));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

}  // namespace
