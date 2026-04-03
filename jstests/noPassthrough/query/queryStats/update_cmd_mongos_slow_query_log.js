/**
 * Test to verify queryShapeHash behavior in slow query logs for update commands on mongos.
 *
 * Behavior:
 *   - Single update: queryShapeHash is present in slow query logs on mongos.
 *   - Batched update: queryShapeHash is not present in slow query logs on mongos.
 *
 * @tags: [requires_fcv_90]
 */

import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getQueryShapeHashFromSlowLogs,
    getSlowQueryLogs,
    resetUpdateTestCollections,
} from "jstests/libs/query/query_stats_utils.js";
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

describe("Mongos - Single vs Batched Updates Query Shape Hash", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 1,
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

        this.collNames = {
            unsharded: "unsharded_coll",
            sharded: "sharded_coll",
        };

        // Set slow query threshold to -1 so every query gets logged.
        this.routerDB.setProfilingLevel(0, -1);
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
        });
    });

    describe("Single Updates", function () {
        // For single updates, we only check "Slow query" logs (not in-progress) because
        // queryShapeHash is computed inside cluster::write() and may not be available
        // when early "Slow in-progress query" logs are emitted.
        it("single update on unsharded collection: mongos has queryShapeHash", function () {
            const comment = `single_update_mongos_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    update: this.collNames.unsharded,
                    updates: [{q: {v: 1}, u: {$set: {updated: true}}}],
                    comment: comment,
                }),
            );

            const hash = getQueryShapeHashFromSlowLogs({testDB: this.routerDB, queryComment: comment});
            assert.neq(
                hash,
                null,
                "queryShapeHash should be present on mongos for single update on unsharded collection",
            );
        });

        it("single update on sharded collection: mongos has queryShapeHash", function () {
            const comment = `single_update_sharded_mongos_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    update: this.collNames.sharded,
                    updates: [{q: {v: 1}, u: {$set: {updated: true}}}],
                    comment: comment,
                }),
            );

            const hash = getQueryShapeHashFromSlowLogs({testDB: this.routerDB, queryComment: comment});
            assert.neq(
                hash,
                null,
                "queryShapeHash should be present on mongos for single update on sharded collection",
            );
        });
    });

    describe("Batched Updates", function () {
        // For batched updates, we check BOTH "Slow query" and "Slow in-progress query" logs
        // to ensure queryShapeHash is excluded from all slow query logging.
        it("batched update on unsharded collection: mongos log should NOT have queryShapeHash", function () {
            const comment = `batched_update_mongos_${UUID().toString()}`;

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

            // Check both "Slow query" and "Slow in-progress query" logs.
            const mongosLogs = getSlowQueryLogs(this.routerDB, comment, {includeInProgress: true});

            assert.gte(mongosLogs.length, 1, "At least one log - there may be a slow in-progress log");
            for (const log of mongosLogs) {
                assert.eq(
                    log.attr.queryShapeHash,
                    undefined,
                    `queryShapeHash should NOT be present on mongos for batched update (${
                        log.msg
                    }): ${tojson(log.attr)}`,
                );
            }
        });

        it("batched update on sharded collection: mongos log should NOT have queryShapeHash", function () {
            const comment = `batched_update_sharded_mongos_${UUID().toString()}`;

            assert.commandWorked(
                this.routerDB.runCommand({
                    update: this.collNames.sharded,
                    updates: [
                        {q: {v: 1}, u: {$set: {batch1: true}}},
                        {q: {v: 2}, u: {$inc: {counter: 1}}},
                    ],
                    comment: comment,
                }),
            );

            // Check both "Slow query" and "Slow in-progress query" logs.
            const mongosLogs = getSlowQueryLogs(this.routerDB, comment, {includeInProgress: true});

            assert.gte(mongosLogs.length, 1, "At least one log - there may be a slow in-progress log");
            for (const log of mongosLogs) {
                assert.eq(
                    log.attr.queryShapeHash,
                    undefined,
                    `queryShapeHash should NOT be present on mongos for batched update on sharded collection (${
                        log.msg
                    }): ${tojson(log.attr)}`,
                );
            }
        });
    });
});
