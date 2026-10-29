// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
    exec::agg::MockStage::setSource_forTest(convertIndexStatsStage.get(), stage.get());
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
    exec::agg::MockStage::setSource_forTest(convertIndexStatsStage.get(), stage.get());
    auto next = convertIndexStatsStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.getDocument(),
                       Document(fromjson("{spec: {name: 'threeFieldIndex', key: {m: 1, t: 1, "
                                         "metric: 1}}, key: {m: 1, t: 1, metric: 1}}")));
}

TEST_F(InternalConvertBucketIndexStatsTest, TestGetNextSkipsFailedConversionWithoutPausing) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const auto timeseriesOptions = TimeseriesIndexConversionOptions{"t"};
    auto convertIndexStatsStage = std::make_unique<exec::agg::InternalConvertBucketIndexStatsStage>(
        DocumentSourceInternalConvertBucketIndexStats::kStageName, expCtx, timeseriesOptions);

    // First document has a key that cannot be converted to time-series format (will produce an
    // empty result from makeTimeseriesIndexStats). Second document is a valid index stats entry.
    auto stage = exec::agg::MockStage::createForTest(
        {"{spec: {name: 'unconvertibleIndex', key: {a: 1}}}",
         "{spec: {name: 'validIndex', key: {'control.min.t': 1, 'control.max.t': 1, "
         "'control.min.metric': 1, 'control.max.metric': 1}}}"},
        expCtx);
    exec::agg::MockStage::setSource_forTest(convertIndexStatsStage, stage.get());

    // The unconvertible document should be silently skipped (not returned as PauseExecution).
    auto next = convertIndexStatsStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        next.getDocument(),
        Document(fromjson(
            "{spec: {name: 'validIndex', key: {t: 1, metric: 1}}, key: {t: 1, metric: 1}}")));

    // Should be EOF now.
    ASSERT_TRUE(convertIndexStatsStage->getNext().isEOF());
}

TEST_F(InternalConvertBucketIndexStatsTest, TestGetNextSkipsAllFailedConversions) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const auto timeseriesOptions = TimeseriesIndexConversionOptions{"t"};
    auto convertIndexStatsStage = std::make_unique<exec::agg::InternalConvertBucketIndexStatsStage>(
        DocumentSourceInternalConvertBucketIndexStats::kStageName, expCtx, timeseriesOptions);

    // All documents fail conversion.
    auto stage = exec::agg::MockStage::createForTest(
        {"{spec: {name: 'idx1', key: {a: 1}}}", "{spec: {name: 'idx2', key: {b: 1}}}"}, expCtx);
    exec::agg::MockStage::setSource_forTest(convertIndexStatsStage, stage.get());

    // Should skip both unconvertible documents and return EOF.
    ASSERT_TRUE(convertIndexStatsStage->getNext().isEOF());
}

}  // namespace
}  // namespace mongo
