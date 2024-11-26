/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/explain_verbosity_gen.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
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
    auto internalCmd = BSON("find"
                            << "test"
                            << "filter" << BSON("a" << 1) << "lsid"
                            << BSON("id" << mongo::UUID::gen()));
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
    auto internalCmd = BSON("find"
                            << "test"
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
