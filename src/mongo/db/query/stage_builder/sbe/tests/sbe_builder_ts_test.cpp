/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

class SbeTimeseriesTest : public GoldenSbeStageBuilderTestFixture {
public:
    void setUp() override {
        GoldenSbeStageBuilderTestFixture::setUp();
        _expCtx = new ExpressionContextForTest();
    }

    void tearDown() override {
        _expCtx = nullptr;
        GoldenSbeStageBuilderTestFixture::tearDown();
    }

protected:
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

template <typename T, typename... Args>
std::unique_ptr<T> makeProjNode(boost::intrusive_ptr<ExpressionContext> expCtx,
                                std::unique_ptr<QuerySolutionNode> child,
                                BSONObj projection,
                                Args... args) {
    auto emptyMatchExpression =
        unittest::assertGet(MatchExpressionParser::parse(BSONObj{}, expCtx));
    auto projectionAst = projection_ast::parseAndAnalyze(expCtx, projection, ProjectionPolicies{});

    return std::make_unique<T>(
        std::move(child), emptyMatchExpression.get(), projectionAst, args...);
}

// Stages under tests do not require 'control.min' and 'control.max' fields to be present though
// they are mandatory fields. This data is not valid timeseries data.
const BSONObj bucketWithMeta1 = fromjson(R"(
{
    "_id" : ObjectId("649f0704230f18da067519c4"),
    "control" : {"version" : 1},
    "meta" : "A",
    "data" : {
        "_id" : {"0" : 0, "1": 1},
        "a" : {"0" : 9, "1": 0},
        "time" : {"0" : {$date: "2025-01-13T16:47:09.512Z"}, "1" : {$date: "2025-02-23T16:47:09.512Z"}}
    }
})");
const BSONObj bucketWithMeta2 = fromjson(R"(
{
    "_id" : ObjectId("649f0704c3d83a4c3fe91689"),
    "control" : {"version" : 1},
    "meta" : "B",
    "data" : {
        "time" : {"0" : {$date: "2025-02-23T16:47:38.692Z"}, "1" : {$date: "2025-02-23T16:47:47.918Z"}},
        "_id" : {"0" : 3, "1" : 4},
        "a" : {"0" : 100, "1" : 101}
    }
})");

const BSONObj bucketWithNoMeta1 = fromjson(R"(
{
	"_id" : ObjectId("64a5cb841ade1be79f4cc8c7"),
	"control" : {"version" : 1},
	"data" : {
        "a" : {"0" : 11, "1" : 22, "2": 33},
        "b" : {"0" : {"a": {"a": 1, "b": 2}}, "2" : {"a": {"b": 1}}},
		"time" : {
			"0" : {$date: "2025-02-05T19:59:28.339Z"},
			"1" : {$date: "2025-02-05T19:59:38.396Z"},
			"2" : {$date: "2025-02-05T19:59:50.772Z"}
		}
	}
})");
const BSONObj bucketWithNoMeta2 = fromjson(R"(
{
	"_id" : ObjectId("64a5cb841ade1be79f4cc8c7"),
	"control" : {"version" : 1},
	"data" : {
        "a" : {"0" : 1, "1" : 2, "2": 3},
        "b" : {"0" : {"a": {"a": 1, "b": 1}}, "1" : {"a": {"b": 2}}, "2": {"a": 1}},
		"time" : {
			"0" : {$date: "2025-02-06T19:59:28.339Z"},
			"1" : {$date: "2025-02-06T19:59:38.396Z"},
			"2" : {$date: "2025-02-06T19:59:50.772Z"}
		}
	}
})");

TEST_F(SbeTimeseriesTest, TestSimpleMatch) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(bucketWithMeta1), BSON_ARRAY(bucketWithMeta2)};
    auto vsNode =
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false);

    BSONObj query = fromjson(R"({time: {$gt: {$date: "2025-02-23T16:47:38.692Z"}}})");
    auto matchExpr = uassertStatusOK(MatchExpressionParser::parse(query, _expCtx));

    auto unpackNode = std::make_unique<UnpackTsBucketNode>(
        std::move(vsNode),
        timeseries::BucketSpec{"time" /* timeField */,
                               std::string("tag") /* metaField */,
                               {"_id", "a", "time"} /* fields */,
                               timeseries::BucketSpec::Behavior::kInclude},
        nullptr /* eventFilter */,
        nullptr /* wholeBucketFilter */,
        true /* includeMeta */);

    auto matchNode = std::make_unique<MatchNode>(std::move(unpackNode), std::move(matchExpr));
    auto projectNode = makeProjNode<ProjectionNodeDefault>(
        _expCtx, std::move(matchNode), BSON("a" << 1 << "tag" << 1 << "_id" << 0));

    runTest(std::move(projectNode), BSONArray(fromjson("[{ a: 101, tag: 'B'}]")));
}

TEST_F(SbeTimeseriesTest, TestMatch) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(bucketWithMeta1), BSON_ARRAY(bucketWithMeta2)};
    auto vsNode =
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false);

    BSONObj query = fromjson(R"({time: {$gt: {$date: "2025-02-23T16:47:38.692Z"}}})");
    auto matchExpr = uassertStatusOK(MatchExpressionParser::parse(query, _expCtx));

    auto unpackNode = std::make_unique<UnpackTsBucketNode>(
        std::move(vsNode),
        timeseries::BucketSpec{"time" /* timeField */,
                               std::string("tag") /* metaField */,
                               {"_id", "a", "time"} /* fields */,
                               timeseries::BucketSpec::Behavior::kInclude},
        std::move(matchExpr),
        nullptr /* wholeBucketFilter */,
        true /* includeMeta */);

    auto projectNode = makeProjNode<ProjectionNodeDefault>(
        _expCtx, std::move(unpackNode), BSON("a" << 1 << "tag" << 1 << "_id" << 0));

    runTest(std::move(projectNode), BSONArray(fromjson("[{ a: 101, tag: 'B'}]")));
}

TEST_F(SbeTimeseriesTest, TestCount) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(bucketWithMeta1), BSON_ARRAY(bucketWithMeta2)};
    auto vsNode =
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false);

    BSONObj query = fromjson(R"({time: {$gt: {$date: "2025-01-23T16:47:38.692Z"}}})");
    auto matchExpr = uassertStatusOK(MatchExpressionParser::parse(query, _expCtx));

    auto unpackNode = std::make_unique<UnpackTsBucketNode>(
        std::move(vsNode),
        timeseries::BucketSpec{"time" /* timeField */,
                               std::string("tag") /* metaField */,
                               {"_id", "a", "time"} /* fields */,
                               timeseries::BucketSpec::Behavior::kInclude},
        std::move(matchExpr),
        nullptr /* wholeBucketFilter */,
        false /* includeMeta */);

    auto bson = fromjson("{count: {$count: {}}}");
    VariablesParseState vps = _expCtx->variablesParseState;
    auto groupNode = std::make_unique<GroupNode>(
        std::move(unpackNode),
        ExpressionConstant::create(_expCtx.get(), Value(BSONNULL)),
        std::vector<AccumulationStatement>{
            AccumulationStatement::parseAccumulationStatement(_expCtx.get(), bson["count"], vps)},
        false /*doingMerge*/,
        false /*willBeMerged*/,
        true /*shouldProduceBson*/);

    auto projectNode = makeProjNode<ProjectionNodeDefault>(
        _expCtx, std::move(groupNode), BSON("count" << 1 << "_id" << 0));

    runTest(std::move(projectNode), BSONArray(fromjson("[{count : 3}]")));
}

TEST_F(SbeTimeseriesTest, TestGroupAvg) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(bucketWithMeta1), BSON_ARRAY(bucketWithMeta2)};
    auto vsNode =
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false);

    BSONObj query = fromjson(R"({a: {$gt: 0}})");
    auto matchExpr = uassertStatusOK(MatchExpressionParser::parse(query, _expCtx));

    auto unpackNode = std::make_unique<UnpackTsBucketNode>(
        std::move(vsNode),
        timeseries::BucketSpec{"time" /* timeField */,
                               std::string("tag") /* metaField */,
                               {"_id", "a", "time"} /* fields */,
                               timeseries::BucketSpec::Behavior::kInclude},
        std::move(matchExpr),
        nullptr /* wholeBucketFilter */,
        false /* includeMeta */);

    auto bson = fromjson("{average: {$avg: '$a'}}");
    VariablesParseState vps = _expCtx->variablesParseState;
    auto groupNode = std::make_unique<GroupNode>(
        std::move(unpackNode),
        ExpressionConstant::create(_expCtx.get(), Value(BSONNULL)),
        std::vector<AccumulationStatement>{
            AccumulationStatement::parseAccumulationStatement(_expCtx.get(), bson["average"], vps)},
        false /*doingMerge*/,
        false /*willBeMerged*/,
        true /*shouldProduceBson*/);

    auto projectNode =
        makeProjNode<ProjectionNodeDefault>(_expCtx, std::move(groupNode), BSON("_id" << 0));

    runTest(std::move(projectNode), BSONArray(fromjson("[{average: 70}]")));
}

TEST_F(SbeTimeseriesTest, TestGroupTopN) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(bucketWithMeta1), BSON_ARRAY(bucketWithMeta2)};
    auto vsNode =
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false);

    BSONObj query = fromjson(R"({time: {$gt: {$date: "2025-01-23T16:47:38.692Z"}}})");
    auto matchExpr = uassertStatusOK(MatchExpressionParser::parse(query, _expCtx));

    auto unpackNode = std::make_unique<UnpackTsBucketNode>(
        std::move(vsNode),
        timeseries::BucketSpec{"time" /* timeField */,
                               std::string("tag") /* metaField */,
                               {"_id", "a", "time"} /* fields */,
                               timeseries::BucketSpec::Behavior::kInclude},
        std::move(matchExpr),
        nullptr /* wholeBucketFilter */,
        true /* includeMeta */);

    auto bson = fromjson("{topN: {$topN: {output: {tag: '$tag'}, sortBy: {a: 1}, n: 2}}}");
    VariablesParseState vps = _expCtx->variablesParseState;
    auto groupNode = std::make_unique<GroupNode>(
        std::move(unpackNode),
        ExpressionConstant::create(_expCtx.get(), Value(BSONNULL)),
        std::vector<AccumulationStatement>{
            AccumulationStatement::parseAccumulationStatement(_expCtx.get(), bson["topN"], vps)},
        false /*doingMerge*/,
        false /*willBeMerged*/,
        true /*shouldProduceBson*/);

    auto projectNode =
        makeProjNode<ProjectionNodeDefault>(_expCtx, std::move(groupNode), BSON("_id" << 0));

    runTest(std::move(projectNode), BSONArray(fromjson("[{topN: [{tag : 'A'}, {tag : 'B'}]}]")));
}

TEST_F(SbeTimeseriesTest, TestGroupMax) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(bucketWithMeta1), BSON_ARRAY(bucketWithMeta2)};
    auto vsNode =
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false);

    BSONObj query = fromjson(R"({time: {$gt: {$date: "2025-01-23T16:47:38.692Z"}}})");
    auto matchExpr = uassertStatusOK(MatchExpressionParser::parse(query, _expCtx));

    auto unpackNode = std::make_unique<UnpackTsBucketNode>(
        std::move(vsNode),
        timeseries::BucketSpec{"time" /* timeField */,
                               std::string("tag") /* metaField */,
                               {"_id", "a", "time"} /* fields */,
                               timeseries::BucketSpec::Behavior::kInclude},
        std::move(matchExpr),
        nullptr /* wholeBucketFilter */,
        true /* includeMeta */);

    auto bson = fromjson("{max: {$max: '$a'}}");
    VariablesParseState vps = _expCtx->variablesParseState;
    auto groupByExpression = ExpressionFieldPath::parse(_expCtx.get(), "$tag", vps);
    auto groupNode = std::make_unique<GroupNode>(
        std::move(unpackNode),
        groupByExpression,
        std::vector<AccumulationStatement>{
            AccumulationStatement::parseAccumulationStatement(_expCtx.get(), bson["max"], vps)},
        false /*doingMerge*/,
        false /*willBeMerged*/,
        true /*shouldProduceBson*/);

    auto projectNode =
        makeProjNode<ProjectionNodeDefault>(_expCtx, std::move(groupNode), BSON("_id" << 0));
    auto sortNode = std::make_unique<SortNodeDefault>(std::move(projectNode),
                                                      BSON("max" << 1) /* pattern */,
                                                      -1 /* limit */,
                                                      LimitSkipParameterization::Disabled);

    runTest(std::move(sortNode), BSONArray(fromjson("[{max: 0}, {max: 101}]")));
}

TEST_F(SbeTimeseriesTest, TestGroupSum) {
    auto docs =
        std::vector<BSONArray>{BSON_ARRAY(bucketWithNoMeta1), BSON_ARRAY(bucketWithNoMeta2)};
    auto vsNode =
        std::make_unique<VirtualScanNode>(docs, VirtualScanNode::ScanType::kCollScan, false);

    BSONObj query = fromjson(R"({time: {$gt: {$date: "2025-02-05T19:59:28.339Z"}}})");
    auto matchExpr = uassertStatusOK(MatchExpressionParser::parse(query, _expCtx));

    auto unpackNode = std::make_unique<UnpackTsBucketNode>(
        std::move(vsNode),
        timeseries::BucketSpec{"time" /* timeField */,
                               boost::none /* metaField */,
                               {"_id", "a", "b", "time"} /* fields */,
                               timeseries::BucketSpec::Behavior::kInclude},
        std::move(matchExpr),
        nullptr /* wholeBucketFilter */,
        false /* includeMeta */);

    auto bson = fromjson("{sum: {$sum: '$a'}}");
    VariablesParseState vps = _expCtx->variablesParseState;
    auto groupByExpression = ExpressionFieldPath::parse(_expCtx.get(), "$b.a.b", vps);
    auto groupNode = std::make_unique<GroupNode>(
        std::move(unpackNode),
        groupByExpression,
        std::vector<AccumulationStatement>{
            AccumulationStatement::parseAccumulationStatement(_expCtx.get(), bson["sum"], vps)},
        false /*doingMerge*/,
        false /*willBeMerged*/,
        true /*shouldProduceBson*/);

    auto sortNode = std::make_unique<SortNodeDefault>(std::move(groupNode),
                                                      BSON("sum" << 1) /* pattern */,
                                                      -1 /* limit */,
                                                      LimitSkipParameterization::Disabled);

    runTest(std::move(sortNode),
            BSONArray(fromjson("[{_id: 2, sum: 2}, {_id: null, sum: 25}, {_id: 1, sum: 34}]")));
}

}  // namespace mongo
