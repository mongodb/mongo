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

#include "mongo/platform/basic.h"

#include <algorithm>

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_plan_cache_stats.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using DocumentSourcePlanCacheStatsTest = AggregationContextFixture;

static const BSONObj kEmptySpecObj = fromjson("{$planCacheStats: {}}");
static const BSONObj kAllHostsFalseSpecObj = fromjson("{$planCacheStats: {allHosts: false}}");
static const BSONObj kAllHostsTrueSpecObj = fromjson("{$planCacheStats: {allHosts: true}}");

/**
 * A MongoProcessInterface used for testing which returns artificial plan cache stats.
 */
class PlanCacheStatsMongoProcessInterface final : public StubMongoProcessInterface {
public:
    PlanCacheStatsMongoProcessInterface(std::vector<BSONObj> planCacheStats)
        : _planCacheStats(std::move(planCacheStats)) {}

    std::vector<BSONObj> getMatchingPlanCacheEntryStats(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const MatchExpression* matchExpr) const override {
        if (!matchExpr) {
            return _planCacheStats;
        }

        std::vector<BSONObj> filteredStats{};
        std::copy_if(_planCacheStats.begin(),
                     _planCacheStats.end(),
                     std::back_inserter(filteredStats),
                     [&matchExpr](const BSONObj& obj) { return matchExpr->matchesBSON(obj); });
        return filteredStats;
    }

    std::string getShardName(OperationContext* opCtx) const override {
        return "testShardName";
    }

    std::string getHostAndPort(OperationContext* opCtx) const override {
        return "testHostName";
    }

private:
    std::vector<BSONObj> _planCacheStats;
};

TEST_F(DocumentSourcePlanCacheStatsTest, ShouldFailToParseIfSpecIsNotObject) {
    const auto specObj = fromjson("{$planCacheStats: 1}");
    ASSERT_THROWS_CODE(
        DocumentSourcePlanCacheStats::createFromBson(specObj.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourcePlanCacheStatsTest, ShouldFailToParseIfSpecIsANonEmptyObject) {
    const auto specObj = fromjson("{$planCacheStats: {unknownOption: 1}}");
    ASSERT_THROWS_CODE(
        DocumentSourcePlanCacheStats::createFromBson(specObj.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourcePlanCacheStatsTest, ShouldFailToParseIfAllHostsTrueInNonShardedContext) {
    ASSERT_THROWS_CODE(DocumentSourcePlanCacheStats::createFromBson(
                           kAllHostsTrueSpecObj.firstElement(), getExpCtx()),
                       AssertionException,
                       4503200);
}

TEST_F(DocumentSourcePlanCacheStatsTest, CanParseAndSerializeSuccessfully) {
    auto stage =
        DocumentSourcePlanCacheStats::createFromBson(kEmptySpecObj.firstElement(), getExpCtx());
    std::vector<Value> serialized;
    stage->serializeToArray(serialized);
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(kAllHostsFalseSpecObj, serialized[0].getDocument().toBson());
}

TEST_F(DocumentSourcePlanCacheStatsTest, CanParseAndSerializeAsExplainSuccessfully) {
    auto stage =
        DocumentSourcePlanCacheStats::createFromBson(kEmptySpecObj.firstElement(), getExpCtx());
    std::vector<Value> serialized;
    stage->serializeToArray(serialized, ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(kAllHostsFalseSpecObj, serialized[0].getDocument().toBson());
}

TEST_F(DocumentSourcePlanCacheStatsTest, CanParseAndSerializeAllHostsSuccessfully) {
    getExpCtx()->fromMongos = true;
    auto stage = DocumentSourcePlanCacheStats::createFromBson(kAllHostsTrueSpecObj.firstElement(),
                                                              getExpCtx());
    std::vector<Value> serialized;
    stage->serializeToArray(serialized);
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(kAllHostsTrueSpecObj, serialized[0].getDocument().toBson());
}

TEST_F(DocumentSourcePlanCacheStatsTest, CanParseAndSerializeAsExplainAllHostsSuccessfully) {
    getExpCtx()->fromMongos = true;
    auto stage = DocumentSourcePlanCacheStats::createFromBson(kAllHostsTrueSpecObj.firstElement(),
                                                              getExpCtx());
    std::vector<Value> serialized;
    stage->serializeToArray(serialized, ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(kAllHostsTrueSpecObj, serialized[0].getDocument().toBson());
}

TEST_F(DocumentSourcePlanCacheStatsTest, SerializesSuccessfullyAfterAbsorbingMatch) {
    auto planCacheStats =
        DocumentSourcePlanCacheStats::createFromBson(kEmptySpecObj.firstElement(), getExpCtx());
    auto match = DocumentSourceMatch::create(fromjson("{foo: 'bar'}"), getExpCtx());
    auto pipeline = Pipeline::create({planCacheStats, match}, getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();
    ASSERT_EQ(1u, pipeline->getSources().size());

    auto serialized = pipeline->serialize();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(kAllHostsFalseSpecObj, serialized[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {foo: 'bar'}}"), serialized[1].getDocument().toBson());
}

TEST_F(DocumentSourcePlanCacheStatsTest, SerializesSuccessfullyAfterAbsorbingMatchForExplain) {
    auto planCacheStats =
        DocumentSourcePlanCacheStats::createFromBson(kEmptySpecObj.firstElement(), getExpCtx());
    auto match = DocumentSourceMatch::create(fromjson("{foo: 'bar'}"), getExpCtx());
    auto pipeline = Pipeline::create({planCacheStats, match}, getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();
    ASSERT_EQ(1u, pipeline->getSources().size());

    auto serialized = pipeline->writeExplainOps(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$planCacheStats: {match: {foo: 'bar'}, allHosts: false}}"),
                      serialized[0].getDocument().toBson());
}

TEST_F(DocumentSourcePlanCacheStatsTest, SerializesAllHostsSuccessfullyAfterAbsorbingMatch) {
    getExpCtx()->fromMongos = true;
    auto planCacheStats = DocumentSourcePlanCacheStats::createFromBson(
        kAllHostsTrueSpecObj.firstElement(), getExpCtx());
    auto match = DocumentSourceMatch::create(fromjson("{foo: 'bar'}"), getExpCtx());
    auto pipeline = Pipeline::create({planCacheStats, match}, getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();
    ASSERT_EQ(1u, pipeline->getSources().size());

    auto serialized = pipeline->serialize();
    ASSERT_EQ(2u, serialized.size());
    ASSERT_BSONOBJ_EQ(kAllHostsTrueSpecObj, serialized[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ(fromjson("{$match: {foo: 'bar'}}"), serialized[1].getDocument().toBson());
}

TEST_F(DocumentSourcePlanCacheStatsTest,
       SerializesAllHostsSuccessfullyAfterAbsorbingMatchForExplain) {
    getExpCtx()->fromMongos = true;
    auto planCacheStats = DocumentSourcePlanCacheStats::createFromBson(
        kAllHostsTrueSpecObj.firstElement(), getExpCtx());
    auto match = DocumentSourceMatch::create(fromjson("{foo: 'bar'}"), getExpCtx());
    auto pipeline = Pipeline::create({planCacheStats, match}, getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();
    ASSERT_EQ(1u, pipeline->getSources().size());

    auto serialized = pipeline->writeExplainOps(ExplainOptions::Verbosity::kQueryPlanner);
    ASSERT_EQ(1u, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{$planCacheStats: {match: {foo: 'bar'}, allHosts: true}}"),
                      serialized[0].getDocument().toBson());
}

TEST_F(DocumentSourcePlanCacheStatsTest, RedactsSuccessfullyAfterAbsorbingMatch) {
    auto planCacheStats =
        DocumentSourcePlanCacheStats::createFromBson(kEmptySpecObj.firstElement(), getExpCtx());
    auto match = DocumentSourceMatch::create(fromjson("{foo: 'bar'}"), getExpCtx());
    auto pipeline = Pipeline::create({planCacheStats, match}, getExpCtx());
    ASSERT_EQ(2u, pipeline->getSources().size());

    pipeline->optimizePipeline();
    ASSERT_EQ(1u, pipeline->getSources().size());
    auto serialized = redactToArray(*pipeline->getSources().front());
    ASSERT_EQ(2u, serialized.size());

    ASSERT_BSONOBJ_EQ(kAllHostsFalseSpecObj, serialized[0].getDocument().toBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({"$match":{"HASH<foo>":{"$eq":"?string"}}})",
        serialized[1].getDocument().toBson().getOwned());
}

TEST_F(DocumentSourcePlanCacheStatsTest, ReturnsImmediateEOFWithEmptyPlanCache) {
    getExpCtx()->mongoProcessInterface =
        std::make_shared<PlanCacheStatsMongoProcessInterface>(std::vector<BSONObj>{});
    auto stage =
        DocumentSourcePlanCacheStats::createFromBson(kEmptySpecObj.firstElement(), getExpCtx());
    ASSERT(stage->getNext().isEOF());
    ASSERT(stage->getNext().isEOF());
}

TEST_F(DocumentSourcePlanCacheStatsTest, ReturnsOnlyMatchingStatsAfterAbsorbingMatch) {
    std::vector<BSONObj> stats{BSONObj(),
                               BSON("foo"
                                    << "bar"),
                               BSON("foo"
                                    << "baz"),
                               BSON("foo"
                                    << "bar"
                                    << "match" << true)};
    getExpCtx()->mongoProcessInterface =
        std::make_shared<PlanCacheStatsMongoProcessInterface>(stats);

    auto planCacheStats =
        DocumentSourcePlanCacheStats::createFromBson(kEmptySpecObj.firstElement(), getExpCtx());
    auto match = DocumentSourceMatch::create(fromjson("{foo: 'bar'}"), getExpCtx());
    auto pipeline = Pipeline::create({planCacheStats, match}, getExpCtx());
    pipeline->optimizePipeline();

    ASSERT_BSONOBJ_EQ(pipeline->getNext()->toBson(),
                      BSON("foo"
                           << "bar"
                           << "host"
                           << "testHostName"));
    ASSERT_BSONOBJ_EQ(pipeline->getNext()->toBson(),
                      BSON("foo"
                           << "bar"
                           << "match" << true << "host"
                           << "testHostName"));
    ASSERT(!pipeline->getNext());
}

TEST_F(DocumentSourcePlanCacheStatsTest, ReturnsHostNameWhenNotFromMongos) {
    std::vector<BSONObj> stats{BSON("foo"
                                    << "bar"),
                               BSON("foo"
                                    << "baz")};
    getExpCtx()->mongoProcessInterface =
        std::make_shared<PlanCacheStatsMongoProcessInterface>(stats);

    auto planCacheStats =
        DocumentSourcePlanCacheStats::createFromBson(kEmptySpecObj.firstElement(), getExpCtx());
    auto pipeline = Pipeline::create({planCacheStats}, getExpCtx());
    ASSERT_BSONOBJ_EQ(pipeline->getNext()->toBson(),
                      BSON("foo"
                           << "bar"
                           << "host"
                           << "testHostName"));
    ASSERT_BSONOBJ_EQ(pipeline->getNext()->toBson(),
                      BSON("foo"
                           << "baz"
                           << "host"
                           << "testHostName"));
    ASSERT(!pipeline->getNext());
}

TEST_F(DocumentSourcePlanCacheStatsTest, ReturnsShardAndHostNameWhenFromMongos) {
    std::vector<BSONObj> stats{BSON("foo"
                                    << "bar"),
                               BSON("foo"
                                    << "baz")};
    getExpCtx()->mongoProcessInterface =
        std::make_shared<PlanCacheStatsMongoProcessInterface>(stats);
    getExpCtx()->fromMongos = true;

    auto planCacheStats =
        DocumentSourcePlanCacheStats::createFromBson(kEmptySpecObj.firstElement(), getExpCtx());
    auto pipeline = Pipeline::create({planCacheStats}, getExpCtx());
    ASSERT_BSONOBJ_EQ(pipeline->getNext()->toBson(),
                      BSON("foo"
                           << "bar"
                           << "host"
                           << "testHostName"
                           << "shard"
                           << "testShardName"));
    ASSERT_BSONOBJ_EQ(pipeline->getNext()->toBson(),
                      BSON("foo"
                           << "baz"
                           << "host"
                           << "testHostName"
                           << "shard"
                           << "testShardName"));
    ASSERT(!pipeline->getNext());
}

}  // namespace mongo
