/**
 * Test to confirm queryShapeHash is outputted in mongod slow query logs for insert commands.
 *
 * @tags: [featureFlagQueryStatsInsert]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {getQueryShapeHashFromSlowLogs, getQueryShapeHashSetFromSlowLogs} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("Query Shape Hash in mongod slow query logs for insert commands", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 2,
            mongos: 1,
            mongosOptions: {
                setParameter: {internalQueryStatsWriteCmdSampleRate: 1},
            },
            shardOptions: {
                setParameter: {internalQueryStatsWriteCmdSampleRate: 1},
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

        // Shard the sharded collection on _id.
        assert.commandWorked(
            this.routerDB.adminCommand({
                shardCollection: `${this.dbName}.${this.collNames.sharded}`,
                key: {_id: 1},
            }),
        );
        assert.commandWorked(
            this.routerDB.adminCommand({
                split: `${this.dbName}.${this.collNames.sharded}`,
                middle: {_id: 0},
            }),
        );
        assert.commandWorked(
            this.routerDB.adminCommand({
                moveChunk: `${this.dbName}.${this.collNames.sharded}`,
                find: {_id: 1},
                to: this.st.shard1.shardName,
            }),
        );

        // Set slow query threshold to -1 so every query gets logged.
        this.routerDB.setProfilingLevel(0, -1);
        this.shard0DB.setProfilingLevel(0, -1);
        this.shard1DB.setProfilingLevel(0, -1);
    });

    after(function () {
        this.st?.stop();
    });

    beforeEach(function () {
        this.routerDB[this.collNames.unsharded].drop();
        this.routerDB[this.collNames.sharded].deleteMany({});
    });

    describe("Unsharded Collection", function () {
        it("single-doc insert should log queryShapeHash on mongod", function () {
            const comment = `single_insert_unsharded_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    insert: this.collNames.unsharded,
                    documents: [{v: 1, x: 10}],
                    comment: comment,
                }),
            );

            const hash = getQueryShapeHashFromSlowLogs({
                testDB: this.shard0DB,
                queryComment: comment,
                options: {commandType: "insert"},
            });
            assert.neq(hash, null, "queryShapeHash should be present in mongod slow log for insert");
        });

        it("multi-doc insert should log queryShapeHash on mongod", function () {
            const comment = `multi_insert_unsharded_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    insert: this.collNames.unsharded,
                    documents: [{v: 1}, {v: 2}, {v: 3}],
                    comment: comment,
                }),
            );

            const hash = getQueryShapeHashFromSlowLogs({
                testDB: this.shard0DB,
                queryComment: comment,
                options: {commandType: "insert"},
            });
            assert.neq(hash, null, "queryShapeHash should be present in mongod slow log for multi-doc insert");
        });
    });

    describe("Sharded Collection", function () {
        it("insert targeting shard0 should log queryShapeHash on shard0", function () {
            const comment = `single_insert_sharded_shard0_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    insert: this.collNames.sharded,
                    documents: [{_id: -1, v: 1}],
                    comment: comment,
                }),
            );

            const hash = getQueryShapeHashFromSlowLogs({
                testDB: this.shard0DB,
                queryComment: comment,
                options: {commandType: "insert"},
            });
            assert.neq(hash, null, "queryShapeHash should be present on shard0 for targeted insert");
        });

        it("insert targeting shard1 should log queryShapeHash on shard1", function () {
            const comment = `single_insert_sharded_shard1_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    insert: this.collNames.sharded,
                    documents: [{_id: 1, v: 2}],
                    comment: comment,
                }),
            );

            const hash = getQueryShapeHashFromSlowLogs({
                testDB: this.shard1DB,
                queryComment: comment,
                options: {commandType: "insert"},
            });
            assert.neq(hash, null, "queryShapeHash should be present on shard1 for targeted insert");
        });

        it("insert spanning both shards should log queryShapeHash on both shards", function () {
            const comment = `multi_shard_insert_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    insert: this.collNames.sharded,
                    documents: [
                        {_id: -1, v: 1},
                        {_id: 1, v: 2},
                    ],
                    comment: comment,
                }),
            );

            const shard0Hash = getQueryShapeHashFromSlowLogs({
                testDB: this.shard0DB,
                queryComment: comment,
                options: {commandType: "insert"},
            });
            assert.neq(shard0Hash, null, "queryShapeHash should be present on shard0");

            const shard1Hash = getQueryShapeHashFromSlowLogs({
                testDB: this.shard1DB,
                queryComment: comment,
                options: {commandType: "insert"},
            });
            assert.neq(shard1Hash, null, "queryShapeHash should be present on shard1");

            // Both shards should report the same hash for the same insert shape.
            assert.eq(
                shard0Hash,
                shard1Hash,
                "queryShapeHash should be the same on both shards for the same insert shape",
            );
        });
    });
});
