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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using WriteCommandsTest = CatalogTestFixture;

// Simulates the router-side value written into includeQueryStatsMetricsForOpIndex: the router
// offsets its local op index by this amount before sending to a shard, so shard metrics come
// back tagged with the original router batch index.
constexpr int kMetricsBaseIndex = 200;

/**
 * Builds a BSON array of 'count' write op entries. For each position i, calls buildEntry(i) to
 * get the base op fields, then appends includeQueryStatsMetricsForOpIndex = kMetricsBaseIndex + i
 * for positions listed in requestMetricsAt.
 */
template <typename BuildEntryFn>
BSONArray buildOpsWithSelectiveMetrics(int count,
                                       BuildEntryFn buildEntry,
                                       std::initializer_list<int> requestMetricsAt) {
    const std::set<int> metricsPositions(requestMetricsAt);
    BSONArrayBuilder arr;
    for (int i = 0; i < count; ++i) {
        BSONObjBuilder entry;
        entry.appendElements(buildEntry(i));
        if (metricsPositions.count(i)) {
            entry.append("includeQueryStatsMetricsForOpIndex", kMetricsBaseIndex + i);
        }
        arr.append(entry.obj());
    }
    return arr.arr();
}

/**
 * Asserts that 'response' contains a 'queryStatsMetrics' array with entries for exactly the
 * original op indices in 'expectedOriginalIndices'. Each entry must have 'originalOpIndex',
 * 'metrics', 'keysExamined', and 'docsExamined'.
 */
void assertQueryStatsMetrics(const BSONObj& response,
                             std::initializer_list<int> expectedOriginalIndices) {
    ASSERT_TRUE(response.hasField("queryStatsMetrics")) << response;
    const auto metricsArray = response["queryStatsMetrics"].Array();
    ASSERT_EQ(metricsArray.size(), expectedOriginalIndices.size()) << response;

    std::set<int> foundIndices;
    for (const auto& elem : metricsArray) {
        const auto obj = elem.Obj();
        ASSERT_TRUE(obj.hasField("originalOpIndex"));
        ASSERT_TRUE(obj.hasField("metrics"));
        foundIndices.insert(obj["originalOpIndex"].Int());

        const auto metrics = obj["metrics"].Obj();
        ASSERT_TRUE(metrics.hasField("keysExamined"));
        ASSERT_TRUE(metrics.hasField("docsExamined"));
    }

    for (int idx : expectedOriginalIndices) {
        ASSERT_EQ(foundIndices.count(idx), 1u)
            << "Expected originalOpIndex " << idx << " not found in queryStatsMetrics";
    }
}

TEST_F(WriteCommandsTest, UpdateReplyContainsMetricsOnlyWhenRequested) {
    NamespaceString ns =
        NamespaceString::createNamespaceString_forTest("write_commands_test", "updateColl");
    auto opCtx = operationContext();

    ASSERT_OK(createCollection(opCtx, ns.dbName(), BSON("create" << ns.coll())));

    DBDirectClient client(opCtx);
    for (int i = 0; i < 5; ++i)
        client.insert(ns, BSON("_id" << i << "x" << i));

    // Build 5 update ops; request shard metrics for positions 1 and 3.
    auto ops = buildOpsWithSelectiveMetrics(
        5,
        [](int i) { return BSON("q" << BSON("x" << i) << "u" << BSON("x" << (i * 10))); },
        {1, 3});

    BSONObj response;
    ASSERT_TRUE(
        client.runCommand(ns.dbName(), BSON("update" << ns.coll() << "updates" << ops), response));
    ASSERT_OK(getStatusFromCommandResult(response));
    ASSERT_EQ(response["n"].Int(), 5);
    ASSERT_EQ(response["nModified"].Int(), 5);

    assertQueryStatsMetrics(response, {kMetricsBaseIndex + 1, kMetricsBaseIndex + 3});
}

TEST_F(WriteCommandsTest, DeleteReplyContainsMetricsOnlyWhenRequested) {
    NamespaceString ns =
        NamespaceString::createNamespaceString_forTest("write_commands_test", "deleteColl");
    auto opCtx = operationContext();

    ASSERT_OK(createCollection(opCtx, ns.dbName(), BSON("create" << ns.coll())));

    DBDirectClient client(opCtx);
    for (int i = 0; i < 5; ++i)
        client.insert(ns, BSON("_id" << i << "x" << i));

    // Build 5 delete ops; request shard metrics for positions 1 and 3.
    auto ops = buildOpsWithSelectiveMetrics(
        5, [](int i) { return BSON("q" << BSON("_id" << i) << "limit" << 1); }, {1, 3});

    BSONObj response;
    ASSERT_TRUE(
        client.runCommand(ns.dbName(), BSON("delete" << ns.coll() << "deletes" << ops), response));
    ASSERT_OK(getStatusFromCommandResult(response));
    ASSERT_EQ(response["n"].Int(), 5);

    assertQueryStatsMetrics(response, {kMetricsBaseIndex + 1, kMetricsBaseIndex + 3});
}

TEST_F(WriteCommandsTest, UpdateReplyOmitsMetricsWhenNoneRequested) {
    NamespaceString ns =
        NamespaceString::createNamespaceString_forTest("write_commands_test", "updateColl2");
    auto opCtx = operationContext();

    ASSERT_OK(createCollection(opCtx, ns.dbName(), BSON("create" << ns.coll())));

    DBDirectClient client(opCtx);
    client.insert(ns, BSON("_id" << 0 << "x" << 0));

    BSONObj response;
    ASSERT_TRUE(client.runCommand(
        ns.dbName(),
        BSON("update" << ns.coll() << "updates"
                      << BSON_ARRAY(BSON("q" << BSON("x" << 0) << "u" << BSON("x" << 10)))),
        response));
    ASSERT_OK(getStatusFromCommandResult(response));
    ASSERT_EQ(response["n"].Int(), 1);
    ASSERT_EQ(response["nModified"].Int(), 1);
    ASSERT_FALSE(response.hasField("queryStatsMetrics")) << response;
}

TEST_F(WriteCommandsTest, DeleteReplyOmitsMetricsWhenNoneRequested) {
    NamespaceString ns =
        NamespaceString::createNamespaceString_forTest("write_commands_test", "deleteColl2");
    auto opCtx = operationContext();

    ASSERT_OK(createCollection(opCtx, ns.dbName(), BSON("create" << ns.coll())));

    DBDirectClient client(opCtx);
    client.insert(ns, BSON("_id" << 0 << "x" << 0));

    BSONObj response;
    ASSERT_TRUE(client.runCommand(
        ns.dbName(),
        BSON("delete" << ns.coll() << "deletes"
                      << BSON_ARRAY(BSON("q" << BSON("_id" << 0) << "limit" << 1))),
        response));
    ASSERT_OK(getStatusFromCommandResult(response));
    ASSERT_EQ(response["n"].Int(), 1);
    ASSERT_FALSE(response.hasField("queryStatsMetrics")) << response;
}

}  // namespace
}  // namespace mongo
