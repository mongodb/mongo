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
}  // namespace
}  // namespace mongo
