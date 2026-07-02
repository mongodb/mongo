/**
 * Test to confirm queryShapeHash from $queryStats on mongos matches queryShapeHash from mongod
 * slow query logs for update and delete commands.
 */

import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getQueryShapeHashFromSlowLogs,
    getQueryShapeHashSetFromSlowLogs,
    getQueryStatsDeleteCmd,
    getQueryStatsUpdateCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {resetTestCollectionsShardedCluster} from "jstests/libs/query/query_stats_write_cmd_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Test data to insert into collections.
const testDocuments = [
    {v: 1, x: 10, y: "a"},
    {v: 2, x: 20, y: "b"},
    {v: 3, x: 30, y: "c"},
    {v: 4, x: 40, y: "a"},
    {v: 5, x: 50, y: "b"},
    {v: 6, x: 60, y: "c"},
    {v: 7, x: 70, y: "a"},
    {v: 8, x: 80, y: "b"},
];

/**
 * Gets the latest queryShapeHash from $queryStats for a collection using the provided getter.
 */
function getLatestQueryShapeHashFromQueryStats(mongosConn, collName, getQueryStatsFn) {
    const entries = getQueryStatsFn(mongosConn, {
        collName: collName,
        customSort: {"metrics.latestSeenTimestamp": -1},
    });
    if (entries.length === 0) {
        return null;
    }
    return entries[0].queryShapeHash;
}

/**
 * Asserts that the queryShapeHash from $queryStats matches the queryShapeHash from mongod slow
 * query logs.
 *
 * @param {object} mongosConn - The mongos connection.
 * @param {string} collName - The collection name.
 * @param {string} comment - The comment used to identify the query in slow logs.
 * @param {object} mongodDB - The mongod database connection to check slow logs.
 * @param {function} getQueryStatsFn - e.g. getQueryStatsUpdateCmd or getQueryStatsDeleteCmd.
 * @param {string} [testDesc] - Optional description for error messages.
 */
function assertQueryStatsAndMongodHashesMatch(
    mongosConn,
    collName,
    comment,
    mongodDB,
    getQueryStatsFn,
    testDesc = "",
) {
    // Get queryShapeHash from mongos $queryStats.
    const queryStatsHash = getLatestQueryShapeHashFromQueryStats(
        mongosConn,
        collName,
        getQueryStatsFn,
    );
    assert.neq(
        queryStatsHash,
        null,
        `queryShapeHash should be present in $queryStats${testDesc ? " for " + testDesc : ""}`,
    );

    // Get queryShapeHash from mongod slow query logs.
    const mongodHash = getQueryShapeHashFromSlowLogs({testDB: mongodDB, queryComment: comment});
    assert.neq(
        mongodHash,
        null,
        `queryShapeHash should be present in mongod slow query logs${testDesc ? " for " + testDesc : ""}`,
    );

    // Verify they match.
    assert.eq(
        queryStatsHash,
        mongodHash,
        `queryShapeHash mismatch${testDesc ? " for " + testDesc : ""}: ` +
            `$queryStats=${queryStatsHash}, mongod=${mongodHash}`,
    );
}

/**
 * Asserts that for a batch of updates or deletes, the queryShapeHash from $queryStats matches the
 * queryShapeHash from mongod slow for each operation in the batch.
 */
function assertQueryShapeHashMatchForBatches(
    mongosConn,
    getQueryStatsFn,
    collName,
    shard0DB,
    comment,
    commandType,
) {
    // Get all queryStats entries for the command on this collection.
    const entries = getQueryStatsFn(mongosConn, {
        collName: collName,
    });
    assert.eq(entries.length, 3, "Expected 3 queryStats entries");

    // Get all distinct queryShapeHashes from mongod slow query logs.
    // Filter by commandType to exclude the top-level command wrapper entry, which doesn't carry a
    // queryShapeHash.
    const slowLogHashes = getQueryShapeHashSetFromSlowLogs({
        testDB: shard0DB,
        queryComment: comment,
        options: {commandType: commandType},
    });
    assert.eq(slowLogHashes.size, 3, "Should have 3 slow query log hashes.");

    // Verify every queryStats entry's queryShapeHash appears in the mongod slow logs.
    for (const entry of entries) {
        const hash = entry.queryShapeHash;
        assert.neq(hash, null, `queryShapeHash should be present in queryStats entry`, {
            entry,
        });
        assert(
            slowLogHashes.has(hash),
            `queryShapeHash from $queryStats not found in mongod slow query logs`,
            {hash, slowLogHashes: Array.from(slowLogHashes)},
        );
    }
}

describe("QueryShapeHash Consistency: mongos $queryStats vs mongod slow query logs", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 2,
            mongos: 1,
            mongosOptions: {
                setParameter: {internalQueryStatsWriteCmdSampleRate: 1},
            },
        });

        this.dbName = jsTestName();
        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: this.dbName,
                primaryShard: this.st.shard0.shardName,
            }),
        );

        this.routerDB = this.st.s.getDB(this.dbName);
        this.shard0DB = this.st.shard0.getDB(this.dbName);
        this.shard1DB = this.st.shard1.getDB(this.dbName);

        this.collNames = {
            unsharded: "unsharded_coll",
            sharded: "sharded_coll",
        };

        // Set slow query threshold to -1 so every query gets logged.
        this.routerDB.setProfilingLevel(0, -1);
        this.shard0DB.setProfilingLevel(0, -1);
        this.shard1DB.setProfilingLevel(0, -1);
    });

    after(function () {
        this.st.stop();
    });

    beforeEach(function () {
        resetTestCollectionsShardedCluster({
            routerDB: this.routerDB,
            unshardedCollName: this.collNames.unsharded,
            shardedCollName: this.collNames.sharded,
            testDocuments: testDocuments,
            st: this.st,
            splitMiddle: {v: 4},
            moveChunkFind: {v: 5},
        });
    });

    describe("Single Updates", function () {
        it("single update on unsharded collection: queryStats hash matches mongod slow log hash", function () {
            const comment = `single_update_unsharded_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    update: this.collNames.unsharded,
                    updates: [
                        {
                            q: {v: {$gte: 1}, x: {$lt: 50}, y: "a"},
                            u: {
                                $set: {updated: true},
                                $inc: {x: 5},
                                $currentDate: {lastModified: true},
                            },
                            multi: true,
                        },
                    ],
                    comment: comment,
                }),
            );

            assertQueryStatsAndMongodHashesMatch(
                this.st.s,
                this.collNames.unsharded,
                comment,
                this.shard0DB,
                getQueryStatsUpdateCmd,
                "single update on unsharded collection",
            );
        });

        it("single update on sharded collection: queryStats hash matches mongod slow log hash", function () {
            const comment = `single_update_sharded_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    update: this.collNames.sharded,
                    updates: [
                        {
                            q: {v: {$in: [1, 2, 3]}, x: {$exists: true}},
                            u: {$set: {updated: true, status: "processed"}, $unset: {y: ""}},
                        },
                    ],
                    comment: comment,
                }),
            );

            assertQueryStatsAndMongodHashesMatch(
                this.st.s,
                this.collNames.sharded,
                comment,
                this.shard0DB,
                getQueryStatsUpdateCmd,
                "single update on sharded collection",
            );
        });
    });

    describe("No-op Updates", function () {
        it("fully no-op update: $queryStats hash matches mongod slow log hash", function () {
            const comment = `full_noop_update_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    update: this.collNames.sharded,
                    updates: [{q: {v: 1, x: 999}, u: {$set: {}, $unset: {}, $inc: {}}}],
                    comment: comment,
                }),
            );

            assertQueryStatsAndMongodHashesMatch(
                this.st.s,
                this.collNames.sharded,
                comment,
                this.shard0DB,
                getQueryStatsUpdateCmd,
                "fully no-op update",
            );
        });

        it("update with some no-op modifiers.", function () {
            const comment = `partial_noop_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    update: this.collNames.sharded,
                    updates: [{q: {v: 2, x: 999}, u: {$set: {x: 10}, $unset: {}, $inc: {}}}],
                    comment: comment,
                }),
            );

            assertQueryStatsAndMongodHashesMatch(
                this.st.s,
                this.collNames.sharded,
                comment,
                this.shard0DB,
                getQueryStatsUpdateCmd,
                "update with some no-op modifiers",
            );
        });
    });

    describe("Batched Updates", function () {
        it("batched update: every queryStats entry has a matching queryShapeHash in mongod slow logs", function () {
            // Clear the query stats cache so we only see entries from this test.
            resetQueryStatsStore(this.st.s, "1%");

            const comment = `batched_update_${UUID().toString()}`;

            // Run a batched update with multiple different query shapes.
            assert.commandWorked(
                this.routerDB.runCommand({
                    update: this.collNames.unsharded,
                    updates: [
                        {q: {v: 1}, u: {$set: {updated: true}}},
                        {q: {v: {$gt: 5}}, u: {$inc: {x: 1}}, multi: true},
                        {q: {v: 3}, u: {$set: {y: "updated"}, $inc: {x: 100}}},
                    ],
                    comment: comment,
                }),
            );

            assertQueryShapeHashMatchForBatches(
                this.st.s,
                getQueryStatsUpdateCmd,
                this.collNames.unsharded,
                this.shard0DB,
                comment,
                "update", // commandType
            );
        });
    });

    describe("Single Deletes", function () {
        it("single delete on unsharded collection: queryStats hash matches mongod slow log hash", function () {
            const comment = `single_delete_unsharded_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    delete: this.collNames.unsharded,
                    deletes: [{q: {v: {$gte: 1}, x: {$lt: 50}, y: "a"}, limit: 0}],
                    comment: comment,
                }),
            );

            assertQueryStatsAndMongodHashesMatch(
                this.st.s,
                this.collNames.unsharded,
                comment,
                this.shard0DB,
                getQueryStatsDeleteCmd,
                "single delete on unsharded collection",
            );
        });

        it("single delete on sharded collection: queryStats hash matches mongod slow log hash", function () {
            const comment = `single_delete_sharded_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    delete: this.collNames.sharded,
                    deletes: [{q: {v: {$in: [1, 2, 3]}, x: {$exists: true}}, limit: 0}],
                    comment: comment,
                }),
            );

            assertQueryStatsAndMongodHashesMatch(
                this.st.s,
                this.collNames.sharded,
                comment,
                this.shard0DB,
                getQueryStatsDeleteCmd,
                "single delete on sharded collection",
            );
        });
    });

    describe("Batched Deletes", function () {
        it("batched delete: every queryStats entry has a matching queryShapeHash in mongod slow logs", function () {
            // Clear the query stats cache so we only see entries from this test.
            resetQueryStatsStore(this.st.s, "1%");

            const comment = `batched_delete_${UUID().toString()}`;

            // Run a batched delete with multiple different query shapes.
            assert.commandWorked(
                this.routerDB.runCommand({
                    delete: this.collNames.unsharded,
                    deletes: [
                        {q: {v: 1}, limit: 1},
                        {q: {v: {$gt: 5}}, limit: 0},
                        {q: {v: 3, y: "c"}, limit: 1},
                    ],
                    comment: comment,
                }),
            );
            assertQueryShapeHashMatchForBatches(
                this.st.s,
                getQueryStatsDeleteCmd,
                this.collNames.unsharded,
                this.shard0DB,
                comment,
                "remove", // commandType
            );
        });
    });

    // TODO SERVER-119643 Add a test for timeseries updates to confirm the queryShapeHash is the
    // same on mongos and mongod. We currently don't output the queryShapeHash for timeseries
    // updates on mongod.
    // TODO SERVER-120999 Add a test for timeseries deletes.
});
