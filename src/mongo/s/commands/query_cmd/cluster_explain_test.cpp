// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/commands/query_cmd/cluster_explain.h"

#include "mongo/bson/json.h"
#include "mongo/db/query/explain_verbosity_gen.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {
void testPruneGenericArgs(const std::string& genericArg) {
    auto internalCmd = fromjson("{find: 'test', filter: {a: 1}, " + genericArg + "}");
    auto verbosity = explain::VerbosityEnum::kQueryPlanner;
    auto expected =
        fromjson("{explain: {find: 'test', filter: {a: 1}}, verbosity: 'queryPlanner'}");
    ASSERT_BSONOBJ_EQ(ClusterExplain::wrapAsExplain(internalCmd, verbosity), expected);
}

void testPropagateGenericArgs(const std::string& genericArg) {
    auto internalCmd = fromjson("{find: 'test', filter: {a: 1}, " + genericArg + "}");
    auto verbosity = explain::VerbosityEnum::kQueryPlanner;
    auto expected = fromjson(
        "{explain: {find: 'test', filter: {a: 1}}, verbosity: 'queryPlanner', " + genericArg + "}");
    ASSERT_BSONOBJ_EQ(ClusterExplain::wrapAsExplain(internalCmd, verbosity), expected);
}

TEST(ClusterExplainTest, PruneMaxTimeMS) {
    std::string maxTimeMS = "maxTimeMS: 1";
    testPruneGenericArgs(maxTimeMS);
}

TEST(ClusterExplainTest, PruneWriteConcern) {
    std::string writeConcern = "writeConcern: {w: 1}";
    testPruneGenericArgs(writeConcern);
}

TEST(ClusterExplainTest, PruneLsid) {
    auto internalCmd =
        BSON("find" << "test"
                    << "filter" << BSON("a" << 1) << "lsid" << BSON("id" << mongo::UUID::gen()));
    auto verbosity = explain::VerbosityEnum::kQueryPlanner;
    auto expected =
        fromjson("{explain: {find: 'test', filter: {a: 1}}, verbosity: 'queryPlanner'}");
    ASSERT_BSONOBJ_EQ(ClusterExplain::wrapAsExplain(internalCmd, verbosity), expected);
}

TEST(ClusterExplainTest, PruneReadPreference) {
    std::string readPreference = "$queryOptions: {$readPreference: 'secondary'}";
    testPruneGenericArgs(readPreference);
}

TEST(ClusterExplainTest, PruneClusterTime) {
    auto internalCmd = BSON("find" << "test"
                                   << "filter" << BSON("a" << 1) << "$clusterTime"
                                   << BSON("clusterTime" << Timestamp(2, 2)) << "$configTime"
                                   << Timestamp(2, 2) << "$topologyTime" << Timestamp(2, 2));
    auto verbosity = explain::VerbosityEnum::kQueryPlanner;
    auto expected =
        fromjson("{explain: {find: 'test', filter: {a: 1}}, verbosity: 'queryPlanner'}");
    ASSERT_BSONOBJ_EQ(ClusterExplain::wrapAsExplain(internalCmd, verbosity), expected);
}

TEST(ClusterExplainTest, PropagateComment) {
    std::string comment = "comment: 'Quetzlcoatl'";
    testPropagateGenericArgs(comment);
}

TEST(ClusterExplainTest, PropagateReadConcern) {
    std::string readConcern = "readConcern: {level: 'linearizable'}";
    testPropagateGenericArgs(readConcern);
}

AsyncRequestsSender::Response makeShardResponse(const std::string& shardName,
                                                BSONObj executionStats) {
    return {ShardId(shardName),
            executor::RemoteCommandResponse::make_forTest(BSON("executionStats" << executionStats),
                                                          Microseconds(0)),
            boost::none};
}

BSONObj buildExecStatsForShards(const std::vector<AsyncRequestsSender::Response>& responses,
                                boost::optional<int64_t> limit = boost::none,
                                boost::optional<int64_t> skip = boost::none) {
    BSONObjBuilder bob;
    ClusterExplain::buildExecStats_forTest(responses, "SINGLE_SHARD", 0, &bob, limit, skip);
    return bob.obj();
}

TEST(ClusterExplainTest, ExecStatsSumsSpillingTotalsAcrossShards) {
    std::vector<AsyncRequestsSender::Response> responses;
    responses.push_back(
        makeShardResponse("shardA",
                          fromjson("{nReturned: 5, totalKeysExamined: 0,"
                                   " totalDocsExamined: 5, totalSpills: 2,"
                                   " totalSpilledBytes: 100, totalSpilledRecords: 10,"
                                   " totalSpilledDataStorageSize: 40,"
                                   " executionStages: {stage: 'SORT'}}")));
    responses.push_back(
        makeShardResponse("shardB",
                          fromjson("{nReturned: 5, totalKeysExamined: 0,"
                                   " totalDocsExamined: 5, totalSpills: 3,"
                                   " totalSpilledBytes: 200, totalSpilledRecords: 20,"
                                   " totalSpilledDataStorageSize: 60,"
                                   " executionStages: {stage: 'SORT'}}")));

    // A limit is applied to show that the spilling totals are the raw sum of the shards' totals.
    // Unlike 'nReturned', they are not clamped or scaled by the router-side limit.
    auto execStats =
        buildExecStatsForShards(responses, /*limit=*/3)["executionStats"].Obj().getOwned();
    ASSERT_EQ(execStats["nReturned"].numberLong(), 3);
    ASSERT_EQ(execStats["totalSpills"].numberLong(), 5);
    ASSERT_EQ(execStats["totalSpilledBytes"].numberLong(), 300);
    ASSERT_EQ(execStats["totalSpilledRecords"].numberLong(), 30);
    ASSERT_EQ(execStats["totalSpilledDataStorageSize"].numberLong(), 100);

    // The root mongos stage reports the same totals.
    auto stage = execStats["executionStages"].Obj();
    ASSERT_EQ(stage["totalSpills"].numberLong(), 5);
    ASSERT_EQ(stage["totalSpilledBytes"].numberLong(), 300);
}

TEST(ClusterExplainTest, ExecStatsOmitsSpillingTotalsWhenNoShardSpilled) {
    std::vector<AsyncRequestsSender::Response> responses;
    responses.push_back(makeShardResponse("shardA",
                                          fromjson("{nReturned: 5, totalKeysExamined: 0,"
                                                   " totalDocsExamined: 5,"
                                                   " executionStages: {stage: 'COLLSCAN'}}")));

    auto execStats = buildExecStatsForShards(responses)["executionStats"].Obj().getOwned();
    ASSERT_FALSE(execStats.hasField("totalSpills"));
    ASSERT_FALSE(execStats.hasField("totalSpilledBytes"));
    ASSERT_FALSE(execStats.hasField("totalSpilledRecords"));
    ASSERT_FALSE(execStats.hasField("totalSpilledDataStorageSize"));
    ASSERT_FALSE(execStats["executionStages"].Obj().hasField("totalSpills"));
}
}  // namespace
}  // namespace mongo
