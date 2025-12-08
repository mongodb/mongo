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

TEST_F(WriteCommandsTest, UpdateReplyContainsMetricsOnlyWhenRequested) {
    NamespaceString ns =
        NamespaceString::createNamespaceString_forTest("write_commands_test", "updateColl");
    auto opCtx = operationContext();

    // Create the collection.
    ASSERT_OK(createCollection(opCtx, ns.dbName(), BSON("create" << ns.coll())));

    // Insert 5 documents.
    DBDirectClient client(opCtx);
    for (int i = 0; i < 5; ++i) {
        client.insert(ns, BSON("_id" << i << "x" << i));
    }

    // Build an update command with 5 statements.
    // Request metrics for indices 1 and 3 only.
    BSONArrayBuilder updatesBuilder;
    for (int i = 0; i < 5; ++i) {
        BSONObjBuilder updateEntry;
        updateEntry.append("q", BSON("x" << i));
        updateEntry.append("u", BSON("x" << (i * 10)));
        if (i == 1 || i == 3) {
            updateEntry.append("includeQueryStatsMetrics", true);
        }
        updatesBuilder.append(updateEntry.obj());
    }

    BSONObj updateCmd = BSON("update" << ns.coll() << "updates" << updatesBuilder.arr());

    // Run the update command.
    BSONObj response;
    ASSERT_TRUE(client.runCommand(ns.dbName(), updateCmd, response)) << response;
    ASSERT_OK(getStatusFromCommandResult(response));

    // Verify all 5 updates succeeded.
    ASSERT_EQ(response["n"].Int(), 5);
    ASSERT_EQ(response["nModified"].Int(), 5);

    // Verify that queryStatsMetrics is present in the response.
    ASSERT_TRUE(response.hasField("queryStatsMetrics")) << response;

    auto metricsArray = response["queryStatsMetrics"].Array();

    // Should have exactly 2 entries (for indices 1 and 3).
    ASSERT_EQ(metricsArray.size(), 2u) << response;

    // Verify the indices are correct.
    std::set<int> metricsIndices;
    for (const auto& elem : metricsArray) {
        auto metricsObj = elem.Obj();
        ASSERT_TRUE(metricsObj.hasField("index"));
        ASSERT_TRUE(metricsObj.hasField("metrics"));
        metricsIndices.insert(metricsObj["index"].Int());

        // Verify metrics object has expected fields.
        auto metrics = metricsObj["metrics"].Obj();
        ASSERT_TRUE(metrics.hasField("keysExamined"));
        ASSERT_TRUE(metrics.hasField("docsExamined"));
    }

    // Verify we got metrics for exactly indices 1 and 3.
    ASSERT_EQ(metricsIndices.count(1), 1u);
    ASSERT_EQ(metricsIndices.count(3), 1u);
    ASSERT_EQ(metricsIndices.size(), 2u);
}

TEST_F(WriteCommandsTest, UpdateReplyOmitsMetricsWhenNoneRequested) {
    NamespaceString ns =
        NamespaceString::createNamespaceString_forTest("write_commands_test", "updateColl2");
    auto opCtx = operationContext();

    // Create the collection.
    ASSERT_OK(createCollection(opCtx, ns.dbName(), BSON("create" << ns.coll())));

    // Insert a document.
    DBDirectClient client(opCtx);
    client.insert(ns, BSON("_id" << 0 << "x" << 0));

    // Build an update command without requesting metrics.
    BSONObj updateCmd =
        BSON("update" << ns.coll() << "updates"
                      << BSON_ARRAY(BSON("q" << BSON("x" << 0) << "u" << BSON("x" << 10))));

    // Run the update command.
    BSONObj response;
    ASSERT_TRUE(client.runCommand(ns.dbName(), updateCmd, response)) << response;
    ASSERT_OK(getStatusFromCommandResult(response));

    // Verify update succeeded.
    ASSERT_EQ(response["n"].Int(), 1);
    ASSERT_EQ(response["nModified"].Int(), 1);

    // Verify that queryStatsMetrics is NOT present in the response.
    ASSERT_FALSE(response.hasField("queryStatsMetrics")) << response;
}

}  // namespace
}  // namespace mongo

