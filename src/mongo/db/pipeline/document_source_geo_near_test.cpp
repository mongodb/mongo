/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceGeoNearTest = AggregationContextFixture;

TEST_F(DocumentSourceGeoNearTest, FailToParseIfKeyFieldNotAString) {
    auto stageObj = fromjson("{$geoNear: {distanceField: 'dist', near: [0, 0], key: 1}}");
    ASSERT_THROWS_CODE(DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceGeoNearTest, FailToParseIfKeyIsTheEmptyString) {
    auto stageObj = fromjson("{$geoNear: {distanceField: 'dist', near: [0, 0], key: ''}}");
    ASSERT_THROWS_CODE(DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceGeoNearTest, FailToParseIfDistanceMultiplierIsNegative) {
    auto stageObj =
        fromjson("{$geoNear: {distanceField: 'dist', near: [0, 0], distanceMultiplier: -1.0}}");
    ASSERT_THROWS_CODE(DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceGeoNearTest, FailToParseIfLimitFieldSpecified) {
    auto stageObj = fromjson("{$geoNear: {distanceField: 'dist', near: [0, 0], limit: 1}}");
    ASSERT_THROWS_CODE(DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx()),
                       AssertionException,
                       50858);
}

TEST_F(DocumentSourceGeoNearTest, FailToParseIfNumFieldSpecified) {
    auto stageObj = fromjson("{$geoNear: {distanceField: 'dist', near: [0, 0], num: 1}}");
    ASSERT_THROWS_CODE(DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx()),
                       AssertionException,
                       50857);
}

TEST_F(DocumentSourceGeoNearTest, FailToParseIfStartOptionIsSpecified) {
    auto stageObj = fromjson("{$geoNear: {distanceField: 'dist', near: [0, 0], start: {}}}");
    ASSERT_THROWS_CODE(DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx()),
                       AssertionException,
                       50856);
}

TEST_F(DocumentSourceGeoNearTest, CanParseAndSerializeKeyField) {
    auto stageObj = fromjson("{$geoNear: {distanceField: 'dist', near: [0, 0], key: 'a.b'}}");
    auto geoNear = DocumentSourceGeoNear::createFromBson(stageObj.firstElement(), getExpCtx());
    std::vector<Value> serialized;
    geoNear->serializeToArray(serialized);
    ASSERT_EQ(serialized.size(), 1u);
    auto expectedSerialization =
        Value{Document{{"$geoNear",
                        Value{Document{{"key", "a.b"_sd},
                                       {"near", std::vector<Value>{Value{0}, Value{0}}},
                                       {"distanceField", "dist"_sd},
                                       {"query", BSONObj()},
                                       {"spherical", false}}}}}};
    ASSERT_VALUE_EQ(expectedSerialization, serialized[0]);
}
}  // namespace
}  // namespace mongo
