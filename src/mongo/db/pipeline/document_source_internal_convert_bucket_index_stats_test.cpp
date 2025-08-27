/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_internal_convert_bucket_index_stats.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/internal_convert_bucket_index_stats_stage.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {
using InternalConvertBucketIndexStatsTest = AggregationContextFixture;

TEST_F(InternalConvertBucketIndexStatsTest, QueryShapeAndRedaction) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();

    auto stage = std::make_unique<DocumentSourceInternalConvertBucketIndexStats>(
        expCtx, TimeseriesIndexConversionOptions{"timefield"});
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$_internalConvertBucketIndexStats":{"timeField":"HASH<timefield>"}})",
        redact(*stage));

    std::string metaField = "metafield";
    stage = std::make_unique<DocumentSourceInternalConvertBucketIndexStats>(
        expCtx, TimeseriesIndexConversionOptions{"timefield", metaField});
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalConvertBucketIndexStats": {
                "timeField": "HASH<timefield>",
                "metaField": "HASH<metafield>"
            }
        })",
        redact(*stage));
}

TEST_F(InternalConvertBucketIndexStatsTest, TestParseErrorWithWrongSpec) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    BSONObj specObj = fromjson("{$_internalConvertBucketIndexStats: {}}");
    ASSERT_THROWS_CODE(DocumentSourceInternalConvertBucketIndexStats::createFromBson(
                           specObj.firstElement(), expCtx),
                       AssertionException,
                       5480004);

    specObj = fromjson("{$_internalConvertBucketIndexStats: {timeField: 1}}");
    ASSERT_THROWS_CODE(DocumentSourceInternalConvertBucketIndexStats::createFromBson(
                           specObj.firstElement(), expCtx),
                       AssertionException,
                       5480001);

    specObj = fromjson("{$_internalConvertBucketIndexStats: {timeField: 't', metaField: 1}}");
    ASSERT_THROWS_CODE(DocumentSourceInternalConvertBucketIndexStats::createFromBson(
                           specObj.firstElement(), expCtx),
                       AssertionException,
                       5480002);

    specObj = fromjson(
        "{$_internalConvertBucketIndexStats: {timeField: 't', metaField: 'm', unknownParam: "
        "true}}");
    ASSERT_THROWS_CODE(DocumentSourceInternalConvertBucketIndexStats::createFromBson(
                           specObj.firstElement(), expCtx),
                       AssertionException,
                       5480003);
}

TEST_F(InternalConvertBucketIndexStatsTest, TestParseWithValidSpec) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    BSONObj specObj =
        fromjson("{$_internalConvertBucketIndexStats: {timeField: 't', metaField: 'm'}}");
    DocumentSourceInternalConvertBucketIndexStats::createFromBson(specObj.firstElement(), expCtx);
}

TEST_F(InternalConvertBucketIndexStatsTest, TestGetNextWithoutMetaField) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const auto timeseriesOptions = TimeseriesIndexConversionOptions{"t"};
    auto convertIndexStatsStage = std::make_unique<exec::agg::InternalConvertBucketIndexStatsStage>(
        DocumentSourceInternalConvertBucketIndexStats::kStageName, expCtx, timeseriesOptions);

    // Create a mock index spec on the buckets collection.
    auto stage = exec::agg::MockStage::createForTest(
        {"{spec: {name: 'twoFieldIndex', key: {'control.min.t': 1, 'control.max.t': 1, "
         "'control.min.metric': 1, 'control.max.metric': 1}}}"},
        expCtx);
    convertIndexStatsStage->setSource(stage.get());
    auto next = convertIndexStatsStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.getDocument(),
        Document(fromjson(
            "{spec: {name: 'twoFieldIndex', key: {t: 1, metric: 1}}, key: {t: 1, metric: 1}}")));
}

TEST_F(InternalConvertBucketIndexStatsTest, TestGetNextWithMetaField) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const auto timeseriesOptions = TimeseriesIndexConversionOptions{"t", std::string("m")};
    auto convertIndexStatsStage = std::make_unique<exec::agg::InternalConvertBucketIndexStatsStage>(
        DocumentSourceInternalConvertBucketIndexStats::kStageName, expCtx, timeseriesOptions);

    // Create a mock index spec on the buckets collection.
    auto stage = exec::agg::MockStage::createForTest(
        {"{spec: {name: 'threeFieldIndex', key: {'meta': 1, 'control.min.t': 1, 'control.max.t': "
         "1, 'control.min.metric': 1, 'control.max.metric': 1}}}"},
        expCtx);
    convertIndexStatsStage->setSource(stage.get());
    auto next = convertIndexStatsStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{spec: {name: 'threeFieldIndex', key: {m: 1, t: 1, "
                                         "metric: 1}}, key: {m: 1, t: 1, metric: 1}}")));
}

}  // namespace
}  // namespace mongo
