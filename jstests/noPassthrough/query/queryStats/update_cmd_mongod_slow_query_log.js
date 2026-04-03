/**
 * Test to confirm queryShapeHash is outputted on mongod slow query logs for sharded cluster update
 * commands.
 *
 * @tags: [requires_fcv_90]
 */

import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getQueryShapeHashFromSlowLogs,
    getQueryShapeHashSetFromSlowLogs,
    resetUpdateTestCollections,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Test data to insert into collections
const testDocuments = [
    {v: 1, x: 10, y: "a", arr: [1, 2, 3], nested: {a: 1, b: 2}},
    {v: 2, x: 20, y: "b", arr: [4, 5, 6], nested: {a: 3, b: 4}},
    {v: 3, x: 30, y: "c", arr: [7, 8, 9], nested: {a: 5, b: 6}},
    {v: 4, x: 40, y: "a", arr: [1, 2], nested: {a: 7, b: 8}},
    {v: 5, x: 50, y: "b", arr: [3, 4], nested: {a: 9, b: 10}},
    {v: 6, x: 60, y: "c", arr: [5, 6], nested: {a: 11, b: 12}},
    {v: 7, x: 70, y: "a", arr: [7, 8], nested: {a: 13, b: 14}},
    {v: 8, x: 80, y: "b", arr: [9, 10], nested: {a: 15, b: 16}},
];

describe("Query Shape Hash Output Tests", function () {
    before(function () {
        // Set up the sharding test cluster
        this.st = new ShardingTest({
            shards: 2,
            mongos: 1,
            mongosOptions: {
                setParameter: {internalQueryStatsRateLimit: -1, internalQueryStatsWriteCmdSampleRate: 1},
            },
            shardOptions: {
                setParameter: {internalQueryStatsRateLimit: -1, internalQueryStatsWriteCmdSampleRate: 1},
            },
        });

        this.dbName = jsTestName();
        assert.commandWorked(
            this.st.s.adminCommand({enableSharding: this.dbName, primaryShard: this.st.shard0.shardName}),
        );

        this.routerDB = this.st.s.getDB(this.dbName);
        this.shard0DB = this.st.shard0.getDB(this.dbName);
        this.shard1DB = this.st.shard1.getDB(this.dbName);

        this.collNames = {
            unsharded: "unsharded_coll",
            sharded: "sharded_coll",
        };

        // Set slow query threshold to -1 so every query gets logged
        this.routerDB.setProfilingLevel(0, -1);
        this.shard0DB.setProfilingLevel(0, -1);
        this.shard1DB.setProfilingLevel(0, -1);
    });

    after(function () {
        this.st?.stop();
    });

    beforeEach(function () {
        resetUpdateTestCollections({
            routerDB: this.routerDB,
            unshardedCollName: this.collNames.unsharded,
            shardedCollName: this.collNames.sharded,
            testDocuments: testDocuments,
            st: this.st,
            splitMiddle: {v: 4},
            moveChunkFind: {v: 5},
        });
    });

    function runUpdateAndAssertHashOnMongod(ctx, collName, updateSpec, testDescription, isShardedColl = false) {
        const comment = `${testDescription.replace(/\s+/g, "_")}_${UUID().toString()}`;

        jsTest.log.info(`\n=== Testing: ${testDescription} ===`);
        jsTest.log.info(`Collection: ${collName}, Comment: ${comment}`);

        const updateCmd = {
            update: collName,
            updates: Array.isArray(updateSpec) ? updateSpec : [updateSpec],
            comment: comment,
        };

        assert.commandWorked(ctx.routerDB.runCommand(updateCmd));

        const shard0Hash = getQueryShapeHashFromSlowLogs({
            testDB: ctx.shard0DB,
            queryComment: comment,
            options: {commandType: "update"},
        });
        assert.neq(shard0Hash, null, `Shard0 queryShapeHash should not be null: ${testDescription}`);

        if (isShardedColl) {
            const shard1Hash = getQueryShapeHashFromSlowLogs({
                testDB: ctx.shard1DB,
                queryComment: comment,
                options: {commandType: "update"},
            });
            assert.neq(shard1Hash, null, `Shard1 queryShapeHash should not be null: ${testDescription}`);
        }
    }

    // ============================================
    // SECTION 1: Updates on unsharded collection.
    // ============================================
    describe("Unsharded Collection", function () {
        it("basic update", function () {
            runUpdateAndAssertHashOnMongod(
                this,
                this.collNames.unsharded,
                {
                    q: {y: "a"},
                    u: {$set: {updated: true}},
                    multi: true,
                },
                "basic update on unsharded",
            );
        });

        it("complex update with multiple features", function () {
            runUpdateAndAssertHashOnMongod(
                this,
                this.collNames.unsharded,
                {
                    q: {
                        $and: [
                            {$or: [{v: {$in: [1, 3, 5]}}, {"nested.a": {$gte: 5}}]},
                            {y: {$regex: "^[ab]$", $options: "i"}},
                            {arr: {$elemMatch: {$gte: 1, $lt: 10}}},
                        ],
                    },
                    u: {
                        $set: {complexUpdate: true, "nested.modified": true},
                        $inc: {x: 1},
                        $currentDate: {lastModified: true},
                    },
                    hint: {v: 1},
                    collation: {locale: "en", strength: 2},
                    multi: true,
                },
                "complex update on unsharded",
            );
        });
    });

    // ============================================
    // SECTION 2: Updates on sharded collection.
    // All tests use multi: true to ensure queries target both shards.
    // ============================================
    describe("Sharded Collection", function () {
        it("basic update targeting both shards", function () {
            runUpdateAndAssertHashOnMongod(
                this,
                this.collNames.sharded,
                {
                    q: {y: "a"},
                    u: {$set: {updated: true}},
                    multi: true,
                },
                "basic update on sharded",
                true, // isShardedColl
            );
        });

        it("complex update with multiple features targeting both shards", function () {
            runUpdateAndAssertHashOnMongod(
                this,
                this.collNames.sharded,
                {
                    q: {
                        $and: [
                            {$or: [{v: {$in: [1, 2, 5, 6]}}, {"nested.a": {$gte: 1}}]},
                            {y: {$regex: "^[abc]$", $options: "i"}},
                            {arr: {$elemMatch: {$gte: 1, $lt: 15}}},
                        ],
                    },
                    u: {
                        $set: {complexUpdate: true, "nested.modified": true},
                        $inc: {x: 1},
                        $currentDate: {lastModified: true},
                    },
                    hint: {v: 1},
                    collation: {locale: "en", strength: 2},
                    multi: true,
                },
                "complex update on sharded",
                true, // isShardedColl
            );
        });
    });

    // ============================================
    // SECTION 3: Batched updates (multiple ops in one command)
    // ============================================
    // Verify that mongod logs queryShapeHash for batched updates.
    // ============================================
    describe("Batched Updates", function () {
        it("batched updates on unsharded collections", function () {
            const comment = `batched_update_unsharded_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    update: this.collNames.unsharded,
                    updates: [
                        {q: {v: 1}, u: {$set: {batch1: true}}},
                        {q: {v: 2}, u: {$inc: {counter: 1}}},
                        {q: {v: 3}, u: {$set: {batch3: "test"}}},
                    ],
                    comment: comment,
                }),
            );
            const shardHashes = getQueryShapeHashSetFromSlowLogs({
                testDB: this.shard0DB,
                queryComment: comment,
                options: {commandType: "update"},
            });
            assert.eq(shardHashes.size, 3, "Shard should log one entry per update op.");
        });

        it("batched updates on sharded collection", function () {
            const comment = `batched_update_sharded_${UUID().toString()}`;

            // Use queries that target both shards: v: 1-3 on shard0, v: 5-7 on shard1
            assert.commandWorked(
                this.routerDB.runCommand({
                    update: this.collNames.sharded,
                    updates: [
                        {q: {v: {$in: [1, 5]}}, u: {$set: {batch1: true}}, multi: true},
                        {q: {v: {$in: [2, 6]}}, u: {$inc: {counter: 1}}, multi: true},
                        {q: {v: {$in: [3, 7]}}, u: {$set: {batch3: "test"}}, multi: true},
                    ],
                    comment: comment,
                }),
            );
            const shard0Hashes = getQueryShapeHashSetFromSlowLogs({
                testDB: this.shard0DB,
                queryComment: comment,
                options: {commandType: "update"},
            });
            const shard1Hashes = getQueryShapeHashSetFromSlowLogs({
                testDB: this.shard1DB,
                queryComment: comment,
                options: {commandType: "update"},
            });
            assert.eq(shard0Hashes.size, 3, "Shard0 should log one entry per update");
            assert.eq(shard1Hashes.size, 3, "Shard1 should log one entry per update");
        });
    });
});
