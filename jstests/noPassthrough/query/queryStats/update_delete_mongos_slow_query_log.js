/**
 * Test to verify queryShapeHash behavior in slow query logs for write commands on mongos.
 *
 * Behavior:
 *   - Single write (one op): queryShapeHash is present in slow query logs on mongos.
 *   - Batched write (multiple ops): queryShapeHash is not present in slow query logs on mongos.
 *
 * Covers: update and delete.
 */

import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    getQueryShapeHashFromSlowLogs,
    getSlowQueryLogs,
} from "jstests/libs/query/query_stats_utils.js";
import {resetTestCollectionsShardedCluster} from "jstests/libs/query/query_stats_write_cmd_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

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

function testMongosHasQueryShapeHash(routerDB, command, comment) {
    assert.commandWorked(routerDB.runCommand(command));
    const hash = getQueryShapeHashFromSlowLogs({
        testDB: routerDB,
        queryComment: comment,
    });
    assert.neq(hash, null, "queryShapeHash should be present on mongos for single write");
}

function testMongosHasNoQueryShapeHashBatch(routerDB, command, comment) {
    assert.commandWorked(routerDB.runCommand(command));
    const mongosLogs = getSlowQueryLogs(routerDB, comment, {includeInProgress: true});
    assert.gte(mongosLogs.length, 1, "At least one log entry expected");
    for (const log of mongosLogs) {
        assert.eq(
            log.attr.queryShapeHash,
            undefined,
            `queryShapeHash should NOT be present for batched write (${log.msg})`,
            {logAttr: log.attr},
        );
    }
}

describe("Mongos - Single vs Batched Write Cmd Query Shape Hash", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 1,
            mongos: 1,
            mongosOptions: {
                setParameter: {
                    internalQueryStatsWriteCmdSampleRate: 1,
                },
            },
            shardOptions: {
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
        this.collNames = {unsharded: "unsharded_coll", sharded: "sharded_coll"};
        this.routerDB.setProfilingLevel(0, -1);
    });

    after(function () {
        this.st?.stop();
    });

    beforeEach(function () {
        resetTestCollectionsShardedCluster({
            routerDB: this.routerDB,
            unshardedCollName: this.collNames.unsharded,
            shardedCollName: this.collNames.sharded,
            testDocuments: testDocuments,
        });
    });

    describe("update", function () {
        it("single update should have queryShapeHash in mongos slow log (unsharded)", function () {
            const comment = "single update unsharded";
            testMongosHasQueryShapeHash(
                this.routerDB,
                {
                    update: this.collNames.unsharded,
                    updates: [{q: {v: 1}, u: {$set: {updated: true}}}],
                    comment,
                },
                comment,
            );
        });

        it("batched update should NOT have queryShapeHash in mongos slow log (unsharded)", function () {
            const comment = "batched update unsharded";
            testMongosHasNoQueryShapeHashBatch(
                this.routerDB,
                {
                    update: this.collNames.unsharded,
                    updates: [
                        {q: {v: 1}, u: {$set: {batch1: true}}},
                        {q: {v: 2}, u: {$inc: {counter: 1}}},
                        {q: {v: 3}, u: {$set: {batch3: "test"}}},
                    ],
                    comment,
                },
                comment,
            );
        });

        it("single update should have queryShapeHash in mongos slow log (sharded)", function () {
            const comment = "single update sharded";
            testMongosHasQueryShapeHash(
                this.routerDB,
                {
                    update: this.collNames.sharded,
                    updates: [{q: {v: 1}, u: {$set: {updated: true}}}],
                    comment,
                },
                comment,
            );
        });

        it("batched update should NOT have queryShapeHash in mongos slow log (sharded)", function () {
            const comment = "batched update sharded";
            testMongosHasNoQueryShapeHashBatch(
                this.routerDB,
                {
                    update: this.collNames.sharded,
                    updates: [
                        {q: {v: 1}, u: {$set: {batch1: true}}},
                        {q: {v: 2}, u: {$inc: {counter: 1}}},
                        {q: {v: 3}, u: {$set: {batch3: "test"}}},
                    ],
                    comment,
                },
                comment,
            );
        });
    });

    describe("delete", function () {
        it("single delete should have queryShapeHash in mongos slow log (unsharded)", function () {
            const comment = "single delete unsharded";
            testMongosHasQueryShapeHash(
                this.routerDB,
                {
                    delete: this.collNames.unsharded,
                    deletes: [{q: {v: 1}, limit: 1}],
                    comment,
                },
                comment,
            );
        });

        it("batched delete should NOT have queryShapeHash in mongos slow log (unsharded)", function () {
            const comment = "batched delete unsharded";
            testMongosHasNoQueryShapeHashBatch(
                this.routerDB,
                {
                    delete: this.collNames.unsharded,
                    deletes: [
                        {q: {v: 1}, limit: 1},
                        {q: {v: 2}, limit: 1},
                        {q: {v: 3}, limit: 1},
                    ],
                    comment,
                },
                comment,
            );
        });

        it("single delete should have queryShapeHash in mongos slow log (sharded)", function () {
            const comment = "single delete sharded";
            testMongosHasQueryShapeHash(
                this.routerDB,
                {
                    delete: this.collNames.sharded,
                    deletes: [{q: {v: 1}, limit: 1}],
                    comment,
                },
                comment,
            );
        });

        it("batched delete should NOT have queryShapeHash in mongos slow log (sharded)", function () {
            const comment = "batched delete sharded";
            testMongosHasNoQueryShapeHashBatch(
                this.routerDB,
                {
                    delete: this.collNames.sharded,
                    deletes: [
                        {q: {v: 1}, limit: 1},
                        {q: {v: 2}, limit: 1},
                        {q: {v: 3}, limit: 1},
                    ],
                    comment,
                },
                comment,
            );
        });
    });
});
