// Tests that 'originalQueryShapeHash' appears in slow query logs on the shards when the command
// originates from the router. 'originalQueryShapeHash' is not expected to appear for getMore and
// explain.
//
// @tags: [
//   requires_profiling,
//   # Profile command doesn't support stepdowns.
//   does_not_support_stepdowns,
//   # Cowardly refusing to run test that interacts with the system profiler as the 'system.profile'
//   # collection is not replicated.
//   does_not_support_causal_consistency,
//   # Does not support transactions as the test issues getMores which cannot be started in a transaction.
//   does_not_support_transactions,
//   requires_fcv_83,
// ]

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getExplainCommand} from "jstests/libs/cmd_object_utils.js";

describe("originalQueryShapeHash appears in slow logs", function () {
    let st;
    let routerDB;
    let shard0DB;

    // Collection configurations to test.
    const collNames = {
        unsharded: "unsharded_coll",
        two_shard: "sharded_two_shards_coll",
        one_shard: "sharded_one_shard_coll",
    };

    before(function () {
        st = new ShardingTest({shards: 2, mongos: 1});
        assert(st.adminCommand({enableSharding: jsTestName(), primaryShard: st.shard0.shardName}));
        routerDB = st.s.getDB(jsTestName());
        shard0DB = st.shard0.getDB(jsTestName());

        // Set up unsharded collection.
        const unshardedColl = routerDB[collNames.unsharded];
        unshardedColl.drop();
        unshardedColl.insertMany([
            {x: 4, y: 1},
            {x: 4, y: 2},
            {x: 5, y: 2},
            {x: 6, y: 3},
        ]);
        assert.commandWorked(routerDB.adminCommand({untrackUnshardedCollection: unshardedColl.getFullName()}));

        // Set up sharded collection on two shards.
        const shardedTwoShardsColl = routerDB[collNames.two_shard];
        shardedTwoShardsColl.drop();
        shardedTwoShardsColl.insertMany([
            {x: 4, y: 1},
            {x: 4, y: 2},
            {x: 5, y: 2},
            {x: 6, y: 3},
            {x: 7, y: 3},
        ]);
        shardedTwoShardsColl.createIndex({y: 1});
        assert.commandWorked(routerDB.adminCommand({shardCollection: shardedTwoShardsColl.getFullName(), key: {y: 1}}));
        // Split and move chunks to ensure data is on both shards.
        assert.commandWorked(routerDB.adminCommand({split: shardedTwoShardsColl.getFullName(), middle: {y: 2}}));
        assert.commandWorked(
            routerDB.adminCommand({
                moveChunk: shardedTwoShardsColl.getFullName(),
                find: {y: 3},
                to: st.shard1.shardName,
            }),
        );

        // Set up sharded collection on one shard.
        const shardedOneShardColl = routerDB[collNames.one_shard];
        shardedOneShardColl.drop();
        shardedOneShardColl.insertMany([
            {x: 4, y: 1},
            {x: 4, y: 2},
            {x: 5, y: 2},
            {x: 6, y: 3},
        ]);
        shardedOneShardColl.createIndex({y: 1});
        assert.commandWorked(routerDB.adminCommand({shardCollection: shardedOneShardColl.getFullName(), key: {y: 1}}));

        // Set slow query threshold to -1 so every query gets logged on the router and shards.
        routerDB.setProfilingLevel(0, -1);
        shard0DB.setProfilingLevel(0, -1);
        const shard1DB = st.shard1.getDB(jsTestName());
        shard1DB.setProfilingLevel(0, -1);
    });

    after(function () {
        st.stop();
    });

    function getSlowQueryLogLines({queryComment, testDB, commandType = null}) {
        const slowQueryLogs = assert
            .commandWorked(testDB.adminCommand({getLog: "global"}))
            .log.map((entry) => JSON.parse(entry))
            .filter((entry) => {
                if (entry.msg !== "Slow query" || !entry.attr || !entry.attr.command) {
                    return false;
                }
                if (commandType === "getMore") {
                    return entry.attr.command.getMore && entry.attr.originatingCommand.comment == queryComment;
                }
                return entry.attr.command.comment == queryComment && !entry.attr.command.getMore;
            });
        jsTest.log.debug("Slow query logs", {queryComment, commandType, slowQueryLogs});
        return slowQueryLogs;
    }

    function getSlowQueryLogLinesFromComment({queryComment, testDB, commandType = null}) {
        const slowQueryLogs = getSlowQueryLogLines({queryComment, testDB, commandType});
        assert(
            slowQueryLogs.length > 0,
            `No slow query log found for comment: ${queryComment}, commandType: ${commandType}`,
        );
        return slowQueryLogs;
    }

    // Asserts whether a hash field should appear in slow query logs. If it should appear, asserts
    // all slow query logs have the same value. The 'key' parameter specifies which field to check:
    // - "queryShapeHash": checks log.attr.queryShapeHash
    // - "originalQueryShapeHash": checks log.attr.command.originalQueryShapeHash
    function assertHashInSlowLogs({comment, testDB, key, shouldAppear = true}) {
        const slowQueryLogs = getSlowQueryLogLinesFromComment({queryComment: comment, testDB});
        let hashValue;
        slowQueryLogs.forEach((slowQueryLog) => {
            const value =
                key === "originalQueryShapeHash"
                    ? slowQueryLog.attr.command.originalQueryShapeHash
                    : slowQueryLog.attr.queryShapeHash;
            assert(
                shouldAppear == Boolean(value),
                `${key} expected to ${shouldAppear ? "appear" : "not appear"} in slow query log. Received: ` +
                    tojson(slowQueryLog),
            );
            if (hashValue) {
                assert.eq(
                    hashValue,
                    value,
                    `Inconsistent ${key} in slow logs. Slow Query Log: ` + tojson(slowQueryLog),
                );
            }
            hashValue = value;
        });
        return hashValue;
    }

    function testQueryShapeHash(query) {
        // Run command to create slow query logs.
        const result = assert.commandWorked(routerDB.runCommand(query));

        // Assert 'originalQueryShapeHash' doesn't appear on the router.
        assertHashInSlowLogs({
            comment: query.comment,
            testDB: routerDB,
            key: "originalQueryShapeHash",
            shouldAppear: false,
        });

        // Assert 'originalQueryShapeHash' appears on the shard(s).
        const originalQueryShapeHash = assertHashInSlowLogs({
            comment: query.comment,
            testDB: shard0DB,
            key: "originalQueryShapeHash",
        });

        // 'originalQueryShapeHash' on the shard should be the same as 'queryShapeHash' on the router.
        const routerQueryShapeHash = assertHashInSlowLogs({
            comment: query.comment,
            testDB: routerDB,
            key: "queryShapeHash",
        });
        assert.eq(
            routerQueryShapeHash,
            originalQueryShapeHash,
            "originalQueryShapeHash and routerQueryShapeHash do not match",
        );

        // If cursor is present, issue getMores and verify 'originalQueryShapeHash' doesn't appear.
        // TODO SERVER-115109 update test to verify 'originalQueryShapeHash' does appear.
        if (result.cursor) {
            const commandCursor = new DBCommandCursor(routerDB, result);
            commandCursor.itcount(); // exhaust the cursor
            const getMoreLogs = [
                ...getSlowQueryLogLines({queryComment: query.comment, testDB: shard0DB, commandType: "getMore"}),
                ...getSlowQueryLogLines({queryComment: query.comment, testDB: routerDB, commandType: "getMore"}),
            ];
            const logsWithOriginalHash = getMoreLogs.filter((log) => log.attr.command.originalQueryShapeHash);
            assert.eq(
                logsWithOriginalHash.length,
                0,
                "getMore should not have originalQueryShapeHash: " + tojson(logsWithOriginalHash),
            );
        }

        // Run explain. 'originalQueryShapeHash' should not appear in the slow query logs for explain.
        const explainComment = query.comment + "_explain";
        const explainQuery = {...query, comment: explainComment};
        assert.commandWorked(routerDB.runCommand(getExplainCommand(explainQuery)));
        assertHashInSlowLogs({
            comment: explainComment,
            testDB: routerDB,
            key: "originalQueryShapeHash",
            shouldAppear: false,
        });
        assertHashInSlowLogs({
            comment: explainComment,
            testDB: shard0DB,
            key: "originalQueryShapeHash",
            shouldAppear: false,
        });
    }

    // Generate test cases for each collection type.
    Object.values(collNames).forEach((collName) => {
        const viewName = collName + "_view";

        describe(`running tests on ${collName}`, function () {
            before(function () {
                // Create a view on top of this collection for view tests.
                routerDB[viewName].drop();
                assert.commandWorked(routerDB.createView(viewName, collName, [{$addFields: {z: 1}}]));
            });

            it("should be reported for find", function () {
                testQueryShapeHash({
                    find: collName,
                    filter: {x: 4},
                    batchSize: 0,
                    comment: `!!find ${collName} test`,
                });
            });

            it("should be reported for find on a view", function () {
                testQueryShapeHash({
                    find: viewName,
                    filter: {x: 4},
                    batchSize: 0,
                    comment: `!!find view ${collName} test`,
                });
            });

            it("should be reported for aggregate", function () {
                testQueryShapeHash({
                    aggregate: collName,
                    pipeline: [{$match: {x: 4}}],
                    cursor: {batchSize: 0},
                    comment: `!!aggregate ${collName} test`,
                });
            });

            it("should be reported for aggregate on a view", function () {
                testQueryShapeHash({
                    aggregate: viewName,
                    pipeline: [{$match: {x: 4}}],
                    cursor: {batchSize: 0},
                    comment: `!!aggregate view ${collName} test`,
                });
            });

            it("should be reported for count", function () {
                testQueryShapeHash({
                    count: collName,
                    query: {x: 4},
                    comment: `!!count ${collName} test`,
                });
            });

            it("should be reported for count on a view", function () {
                testQueryShapeHash({
                    count: viewName,
                    query: {x: 4},
                    comment: `!!count view ${collName} test`,
                });
            });

            it("should be reported for distinct", function () {
                testQueryShapeHash({
                    distinct: collName,
                    key: "x",
                    query: {x: 4},
                    comment: `!!distinct ${collName} test`,
                });
            });

            it("should be reported for distinct on a view", function () {
                testQueryShapeHash({
                    distinct: viewName,
                    key: "x",
                    query: {x: 4},
                    comment: `!!distinct view ${collName} test`,
                });
            });
        });
    });

    // Tests that 'originalQueryShapeHash' cannot be set by users.
    const exampleHash = "90114C24B1F2EB8B3EBCD0F8387199B8C85D3D202DD68126CD9143AFC684E6BF";
    const collName = collNames.unsharded;

    it("should error when find includes originalQueryShapeHash on router", function () {
        const query = {
            find: collName,
            filter: {x: 4},
            originalQueryShapeHash: exampleHash,
        };
        assert.commandFailedWithCode(routerDB.runCommand(query), 10742703);
    });

    it("should error when find includes originalQueryShapeHash on shard", function () {
        const query = {
            find: collName,
            filter: {x: 4},
            originalQueryShapeHash: exampleHash,
        };
        assert.commandFailedWithCode(shard0DB.runCommand(query), 10742702);
    });

    it("should error when aggregate includes originalQueryShapeHash on router", function () {
        const query = {
            aggregate: collName,
            pipeline: [{$match: {x: 4}}],
            cursor: {},
            originalQueryShapeHash: exampleHash,
        };
        assert.commandFailedWithCode(routerDB.runCommand(query), 10742706);
    });

    it("should error when aggregate includes originalQueryShapeHash on shard", function () {
        const query = {
            aggregate: collName,
            pipeline: [{$match: {x: 4}}],
            cursor: {},
            originalQueryShapeHash: exampleHash,
        };
        assert.commandFailedWithCode(shard0DB.runCommand(query), 10742706);
    });

    it("should error when count includes originalQueryShapeHash on router", function () {
        const query = {
            count: collName,
            query: {x: 4},
            originalQueryShapeHash: exampleHash,
        };
        assert.commandFailedWithCode(routerDB.runCommand(query), 10742704);
    });

    it("should error when count includes originalQueryShapeHash on shard", function () {
        const query = {
            count: collName,
            query: {x: 4},
            originalQueryShapeHash: exampleHash,
        };
        assert.commandFailedWithCode(shard0DB.runCommand(query), 10742702);
    });

    it("should error when distinct includes originalQueryShapeHash on router", function () {
        const query = {
            distinct: collName,
            key: "x",
            query: {x: 4},
            originalQueryShapeHash: exampleHash,
        };
        assert.commandFailedWithCode(routerDB.runCommand(query), 10742700);
    });

    it("should error when distinct includes originalQueryShapeHash on shard", function () {
        const query = {
            distinct: collName,
            key: "x",
            query: {x: 4},
            originalQueryShapeHash: exampleHash,
        };
        assert.commandFailedWithCode(shard0DB.runCommand(query), 10742702);
    });
});
