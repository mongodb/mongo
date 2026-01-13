/**
 * Test that query stats properly handles system variables (like $$NOW) in aggregations and find
 * on a sharded cluster. Mongos will add a let expression with the system variable to the command
 * for the shards. This let expression shouldn't be included in query stats because of known reparse
 * errors. However, user defined variables should appear in the query shape on the shards.
 * @tags: [featureFlagQueryStatsForInternalClients]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getLatestQueryStatsEntry, getQueryStatsServerParameters} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("query stats on sharded cluster with system variables", function () {
    let st;
    let mongos;
    let mongosDB;
    let shard0Conn;
    let shard1Conn;
    let coll;
    let dbName;
    let collName;

    before(function () {
        st = new ShardingTest({
            shards: 2,
            mongosOptions: getQueryStatsServerParameters(),
            rsOptions: getQueryStatsServerParameters(),
        });

        dbName = jsTestName();
        collName = jsTestName();
        mongos = st.s;
        mongosDB = mongos.getDB(dbName);
        shard0Conn = st.shard0;
        shard1Conn = st.shard1;

        // Enable sharding and set up the sharded collection.
        assert.commandWorked(mongosDB.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

        coll = mongosDB[collName];
        coll.drop();

        // Insert documents and shard the collection across both shards.
        assert.commandWorked(
            coll.insertMany([
                {_id: 1, x: -3, ts: new Date()},
                {_id: 2, x: -2, ts: new Date()},
                {_id: 3, x: -1, ts: new Date()},
                {_id: 4, x: 1, ts: new Date()},
                {_id: 5, x: 2, ts: new Date()},
            ]),
        );
        coll.createIndex({x: 1});
        assert.commandWorked(mongosDB.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));

        // Split at x: 0 and move positive chunk to shard1.
        assert.commandWorked(mongosDB.adminCommand({split: coll.getFullName(), middle: {x: 0}}));
        assert.commandWorked(
            mongosDB.adminCommand({
                moveChunk: coll.getFullName(),
                find: {x: 1},
                to: st.shard1.shardName,
            }),
        );
    });

    after(function () {
        st.stop();
    });

    function validateQueryShapeAgg({entry, expectedPipeline, expectedLetExpression}) {
        assert.eq(
            entry.key.queryShape.pipeline,
            expectedPipeline,
            "Unexpected query shape on mongos. Received: " + tojson(entry),
        );
        assert.eq(
            entry.key.queryShape.let,
            expectedLetExpression,
            "Unexpected query shape on mongos  Received: " + tojson(entry),
        );
    }

    function validateQueryShapeFind({entry, expectedFilter, expectedLetExpression}) {
        assert.eq(entry.key.queryShape.filter, expectedFilter, "Unexpected query shape. Received: " + tojson(entry));
        assert.eq(
            entry.key.queryShape.let,
            expectedLetExpression,
            "Unexpected query shape. Received: " + tojson(entry),
        );
    }

    it("agg: should only report user defined variables on the shards", function () {
        // Fully execute an aggregation that uses system variables and user-defined variables.
        coll.aggregate([{$match: {x: {$gte: -2}}}, {$project: {ts: 1, now: "$$NOW", then: "$$then"}}], {
            let: {then: ISODate("1999-09-30T04:11:10Z")},
        }).toArray();

        const expectedLetExpression = {"then": "?date"};

        // Validate the query shape on mongos.
        validateQueryShapeAgg({
            entry: getLatestQueryStatsEntry(mongos.getDB(dbName), {collName: collName}),
            expectedPipeline: [
                {$match: {x: {$gte: "?number"}}},
                {$project: {_id: true, ts: true, now: "$$NOW", then: "$$then"}},
            ],
            expectedLetExpression,
        });

        // Validate the query shape from both shards.
        // Mongos seeds the fields in the query that reference variables before forwarding the query
        // to the shards. As a result, the query shape will contain the variable values (e.g., ?date)
        // rather than the variable names (e.g., $$then).
        const expectedPipeline = [
            {$match: {x: {$gte: "?number"}}},
            {$project: {_id: true, ts: true, now: "?date", then: "?date"}},
        ];
        validateQueryShapeAgg({
            entry: getLatestQueryStatsEntry(shard0Conn.getDB(dbName), {collName: collName}),
            expectedPipeline,
            expectedLetExpression,
        });
        validateQueryShapeAgg({
            entry: getLatestQueryStatsEntry(shard1Conn.getDB(dbName), {collName: collName}),
            expectedPipeline,
            expectedLetExpression,
        });
    });

    it("find: let expression should be empty if only system variables are defined", function () {
        assert.commandWorked(
            mongosDB.runCommand({find: collName, filter: {$expr: {$eq: ["$timestamp", "$$CLUSTER_TIME"]}}}),
        );
        const expectedFilter = {$expr: {$eq: ["$timestamp", "$$CLUSTER_TIME"]}};

        // Validate query shape on mongos
        validateQueryShapeFind({
            entry: getLatestQueryStatsEntry(mongos.getDB(dbName), {collName: collName}),
            expectedFilter,
            expectedLetExpression: undefined,
        });

        // Validate query stats from both shards.
        validateQueryShapeFind({
            entry: getLatestQueryStatsEntry(shard0Conn.getDB(dbName), {collName: collName}),
            expectedFilter,
            expectedLetExpression: undefined,
        });
        validateQueryShapeFind({
            entry: getLatestQueryStatsEntry(shard1Conn.getDB(dbName), {collName: collName}),
            expectedFilter,
            expectedLetExpression: undefined,
        });
    });

    it("agg: let expression should be empty if only system variables are defined", function () {
        // Fully execute an aggregation that uses system variables.
        coll.aggregate([{$match: {x: {$gte: -2}}}, {$project: {ts: 1, now: "$$CLUSTER_TIME"}}]).toArray();

        // Validate the query shape on mongos.
        validateQueryShapeAgg({
            entry: getLatestQueryStatsEntry(mongos.getDB(dbName), {collName: collName}),
            expectedPipeline: [
                {$match: {x: {$gte: "?number"}}},
                {$project: {_id: true, ts: true, now: "$$CLUSTER_TIME"}},
            ],
            expectedLetExpression: undefined,
        });

        // Validate the query shape on both shards.
        const expectedLetExpression = {};
        const expectedPipeline = [
            {$match: {x: {$gte: "?number"}}},
            {$project: {_id: true, ts: true, now: "?timestamp"}},
        ];
        validateQueryShapeAgg({
            entry: getLatestQueryStatsEntry(shard0Conn.getDB(dbName), {collName: collName}),
            expectedPipeline,
            expectedLetExpression,
        });
        validateQueryShapeAgg({
            entry: getLatestQueryStatsEntry(shard1Conn.getDB(dbName), {collName: collName}),
            expectedPipeline,
            expectedLetExpression,
        });
    });
});
