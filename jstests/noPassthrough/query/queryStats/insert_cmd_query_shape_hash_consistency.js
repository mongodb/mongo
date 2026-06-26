/**
 * Test to confirm queryShapeHash from $queryStats on mongos matches queryShapeHash from mongod
 * slow query logs for insert commands.
 *
 * @tags: [requires_fcv_90]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getQueryShapeHashFromSlowLogs,
    getQueryStatsInsertCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

/**
 * Gets the latest queryShapeHash from $queryStats for a collection.
 */
function getLatestQueryShapeHashFromQueryStats(mongosConn, collName) {
    const entries = getQueryStatsInsertCmd(mongosConn, {
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
 * query logs for an insert command.
 */
function assertQueryStatsAndMongodHashesMatch(
    mongosConn,
    collName,
    comment,
    mongodDB,
    testDesc = "",
) {
    const queryStatsHash = getLatestQueryShapeHashFromQueryStats(mongosConn, collName);
    assert.neq(
        queryStatsHash,
        null,
        `queryShapeHash should be present in $queryStats${testDesc ? " for " + testDesc : ""}`,
    );

    const mongodHash = getQueryShapeHashFromSlowLogs({
        testDB: mongodDB,
        queryComment: comment,
        options: {commandType: "insert"},
    });
    assert.neq(
        mongodHash,
        null,
        `queryShapeHash should be present in mongod slow query logs${testDesc ? " for " + testDesc : ""}`,
    );

    assert.eq(
        queryStatsHash,
        mongodHash,
        `queryShapeHash mismatch${testDesc ? " for " + testDesc : ""}: ` +
            `$queryStats=${queryStatsHash}, mongod=${mongodHash}`,
    );
}

describe("QueryShapeHash Consistency: mongos $queryStats vs mongod slow query logs (inserts)", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 2,
            mongos: 1,
            mongosOptions: {
                setParameter: {
                    internalQueryStatsWriteCmdSampleRate: 1,
                },
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

        // Shard collection such that documents with _id < 0 land on shard0, _id >=0 land on shard1.
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
        this.st.stop();
    });

    beforeEach(function () {
        this.routerDB[this.collNames.unsharded].drop();
        this.routerDB[this.collNames.sharded].deleteMany({});
        resetQueryStatsStore(this.st.s, "1MB");
    });

    describe("Single Inserts", function () {
        it("insert on unsharded collection: queryStats hash matches mongod slow log hash", function () {
            const comment = `insert_unsharded_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    insert: this.collNames.unsharded,
                    documents: [{v: 1, x: 10, y: "a"}],
                    comment: comment,
                }),
            );

            assertQueryStatsAndMongodHashesMatch(
                this.st.s,
                this.collNames.unsharded,
                comment,
                this.shard0DB,
                "insert on unsharded collection",
            );
        });

        it("insert on sharded collection: queryStats hash matches mongod slow log hash", function () {
            const comment = `insert_sharded_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    insert: this.collNames.sharded,
                    documents: [{_id: -1, v: 1}],
                    comment: comment,
                }),
            );

            assertQueryStatsAndMongodHashesMatch(
                this.st.s,
                this.collNames.sharded,
                comment,
                this.shard0DB,
                "insert on sharded collection",
            );
        });
    });

    describe("Multi-document Inserts", function () {
        it("multi-doc insert on unsharded collection: every queryStats entry has a matching queryShapeHash in mongod slow logs", function () {
            resetQueryStatsStore(this.st.s, "1%");

            const comment = `multi_insert_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    insert: this.collNames.unsharded,
                    documents: [
                        {_id: -1, v: 1},
                        {_id: 2, v: 2},
                        {_id: 3, v: 3},
                    ],
                    comment: comment,
                }),
            );

            // All documents in one insert command share a single shape (?array<?object>), producing one single query stats entry.
            const entries = getQueryStatsInsertCmd(this.st.s, {
                collName: this.collNames.unsharded,
            });
            assert.eq(
                entries.length,
                1,
                "Expected one queryStats entry for the insert: " + tojson(entries),
            );

            // verify queryShapeHash of slow query log on shard0 match router's $queryStats
            assertQueryStatsAndMongodHashesMatch(
                this.st.s,
                this.collNames.unsharded,
                comment,
                this.shard0DB,
                "insert on unsharded collection",
            );
        });

        it("multi-doc insert on sharded collection: every queryStats entry has a matching queryShapeHash in mongod slow logs", function () {
            resetQueryStatsStore(this.st.s, "1%");

            const comment = `multi_insert_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    insert: this.collNames.sharded,
                    documents: [
                        {_id: -1, v: 1},
                        {_id: 2, v: 2},
                        {_id: 3, v: 3},
                    ],
                    comment: comment,
                }),
            );

            // All documents in one insert command share a single shape (?array<?object>), producing one single query stats entry.
            const entries = getQueryStatsInsertCmd(this.st.s, {
                collName: this.collNames.sharded,
            });
            assert.eq(
                entries.length,
                1,
                "Expected one queryStats entry for the insert: " + tojson(entries),
            );

            // verify queryShapeHash of slow query log on shard0 match router's $queryStats
            assertQueryStatsAndMongodHashesMatch(
                this.st.s,
                this.collNames.sharded,
                comment,
                this.shard0DB,
                "insert on sharded collection",
            );

            // verify queryShapeHash of slow query log on shard1 match router's $queryStats
            assertQueryStatsAndMongodHashesMatch(
                this.st.s,
                this.collNames.sharded,
                comment,
                this.shard1DB,
                "insert on sharded collection",
            );
        });
    });
});
