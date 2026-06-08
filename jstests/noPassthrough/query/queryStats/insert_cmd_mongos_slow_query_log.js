/**
 * Test to verify queryShapeHash behavior in slow query logs for insert commands on mongos.
 *
 * For insert commands, mongos computes a single query shape for the entire command (since all
 * documents are shapified as '?array<?object>'), so queryShapeHash should always be present in
 * mongos slow query logs regardless of the number of documents inserted.
 *
 * @tags: [featureFlagQueryStatsInsert]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {getQueryShapeHashFromSlowLogs, getSlowQueryLogs} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("Mongos - Insert Query Shape Hash in Slow Logs", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 1,
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

        this.collNames = {
            unsharded: "unsharded_coll",
            sharded: "sharded_coll",
        };

        assert.commandWorked(
            this.routerDB.adminCommand({
                shardCollection: `${this.dbName}.${this.collNames.sharded}`,
                key: {_id: 1},
            }),
        );

        // Set slow query threshold to -1 so every query gets logged.
        this.routerDB.setProfilingLevel(0, -1);
    });

    after(function () {
        this.st?.stop();
    });

    beforeEach(function () {
        this.routerDB[this.collNames.unsharded].drop();
        this.routerDB[this.collNames.sharded].deleteMany({});
    });

    describe("Single-document Inserts", function () {
        it("single-doc insert on unsharded collection: mongos has queryShapeHash", function () {
            const comment = `single_insert_mongos_unsharded_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    insert: this.collNames.unsharded,
                    documents: [{v: 1}],
                    comment: comment,
                }),
            );

            const hash = getQueryShapeHashFromSlowLogs({testDB: this.routerDB, queryComment: comment});
            assert.neq(
                hash,
                null,
                "queryShapeHash should be present on mongos for single-doc insert on unsharded collection",
            );
        });

        it("single-doc insert on sharded collection: mongos has queryShapeHash", function () {
            const comment = `single_insert_mongos_sharded_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    insert: this.collNames.sharded,
                    documents: [{_id: 1, v: 1}],
                    comment: comment,
                }),
            );

            const hash = getQueryShapeHashFromSlowLogs({testDB: this.routerDB, queryComment: comment});
            assert.neq(
                hash,
                null,
                "queryShapeHash should be present on mongos for single-doc insert on sharded collection",
            );
        });
    });

    describe("Multi-document Inserts", function () {
        // Unlike update commands (where a batched command with multiple 'updates' entries does not
        // have a single shape), an insert command with multiple documents still has one shape
        // ('documents' is always '?array<?object>'). So queryShapeHash should be present.
        it("multi-doc insert on unsharded collection: mongos has queryShapeHash", function () {
            const comment = `multi_insert_mongos_unsharded_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    insert: this.collNames.unsharded,
                    documents: [{v: 1}, {v: 2}, {v: 3}],
                    comment: comment,
                }),
            );

            const mongosLogs = getSlowQueryLogs(this.routerDB, comment);
            assert.gte(mongosLogs.length, 1, "Expected at least one slow query log entry");

            // For inserts, queryShapeHash should always be present since the shape is fixed.
            const hash = getQueryShapeHashFromSlowLogs({testDB: this.routerDB, queryComment: comment});
            assert.neq(
                hash,
                null,
                "queryShapeHash should be present on mongos for multi-doc insert: " + tojson(mongosLogs),
            );
        });

        it("multi-doc insert on sharded collection: mongos has queryShapeHash", function () {
            const comment = `multi_insert_mongos_sharded_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    insert: this.collNames.sharded,
                    documents: [
                        {_id: 1, v: 1},
                        {_id: 2, v: 2},
                    ],
                    comment: comment,
                }),
            );

            const hash = getQueryShapeHashFromSlowLogs({testDB: this.routerDB, queryComment: comment});
            assert.neq(
                hash,
                null,
                "queryShapeHash should be present on mongos for multi-doc insert on sharded collection",
            );
        });
    });
});
