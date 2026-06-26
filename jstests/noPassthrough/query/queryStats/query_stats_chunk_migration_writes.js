/**
 * Tests that write operations performed during chunk migrations are correctly tracked
 * in query stats:
 *
 *   1. Migration batch inserts on the recipient shard
 *   2. Orphan range deletions on the donor shard (TODO(SERVER-129352): Currently not stored in
 *   query stats)
 *   3. Concurrent user-initiated writes (insert, delete, update) on the migrating range during an
 *      active migration
 *
 * @tags: [featureFlagQueryStatsDelete, requires_fcv_90]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {
    getQueryStatsDeleteCmd,
    getQueryStatsInsertCmd,
    getQueryStatsUpdateCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("query stats chunk migration", function () {
    let st;
    let testDB;
    let coll;
    const collName = jsTestName();

    before(function () {
        st = new ShardingTest({
            shards: 2,
            mongosOptions: {setParameter: {internalQueryStatsWriteCmdSampleRate: 1}},
            rsOptions: {setParameter: {internalQueryStatsWriteCmdSampleRate: 1}},
        });
        testDB = st.s.getDB("test");
        coll = testDB[collName];
        assert.commandWorked(
            testDB.adminCommand({
                enableSharding: testDB.getName(),
                primaryShard: st.shard0.shardName,
            }),
        );
    });

    after(function () {
        st?.stop();
    });

    beforeEach(function () {
        // Drop and re-shard so each test starts with a single chunk (MinKey, MaxKey) on shard0.
        coll.drop();
        assert.commandWorked(
            st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}),
        );
        resetQueryStatsStore(st.s, "1MB");
        resetQueryStatsStore(st.shard0, "1MB");
        resetQueryStatsStore(st.shard1, "1MB");
    });

    it("migration batch inserts appear in the recipient shard's query stats store", function () {
        const kDocs = [
            {_id: 1, v: 1},
            {_id: 2, v: 2},
            {_id: 3, v: 3},
            {_id: 4, v: 4},
        ];
        assert.commandWorked(coll.insert(kDocs));

        // Reset shard1 query stats before moveChunk so only migration batch inserts appear there.
        resetQueryStatsStore(st.shard1, "1MB");

        assert.commandWorked(
            st.s.adminCommand({
                moveChunk: coll.getFullName(),
                find: {_id: 1},
                to: st.shard1.shardName,
            }),
        );

        // Use $queryStats rather than the inserts helper - migration batch inserts run
        // under an internal client with no application name, and the helper filters
        // by kShellApplicationName, which would miss these entries.
        const entries = assert.commandWorked(
            st.shard1.adminCommand({
                aggregate: 1,
                pipeline: [
                    {$queryStats: {}},
                    {
                        $match: {
                            "key.queryShape.command": "insert",
                            "key.queryShape.cmdNs.coll": collName,
                        },
                    },
                ],
                cursor: {},
            }),
        ).cursor.firstBatch;

        assert.eq(
            entries.length,
            1,
            "Expected exactly 1 insert query stats entry on the recipient shard",
            {
                entries,
            },
        );

        const totalInserted = entries.reduce((sum, e) => sum + e.metrics.writes.nInserted.sum, 0);
        assert.eq(
            totalInserted,
            kDocs.length,
            "nInserted should equal the number of migrated documents",
            {totalInserted, expected: kDocs.length},
        );
    });

    // TODO(SERVER-129352): Range deletions on the donor go through
    // InternalPlanner::deleteWithShardKeyIndexScan(), which bypasses performDeletes() entirely and
    // therefore never registers query stats. Update this test when range deletions are instrumented.
    it("orphan range deletions are not tracked in query stats on the donor shard", function () {
        const kDocs = [
            {_id: 1, v: 1},
            {_id: 2, v: 2},
            {_id: 3, v: 3},
            {_id: 4, v: 4},
        ];
        assert.commandWorked(coll.insert(kDocs));

        resetQueryStatsStore(st.shard0, "1MB");

        // Migrate the chunk to shard1. shard0 becomes the donor; async range deletion fires.
        assert.commandWorked(
            st.s.adminCommand({
                moveChunk: coll.getFullName(),
                find: {_id: 1},
                to: st.shard1.shardName,
            }),
        );

        // Wait for range deletion on shard0 to drain.
        const shard0Primary = st.rs0.getPrimary();
        assert.soon(
            () => shard0Primary.getDB("config").rangeDeletions.find().itcount() === 0,
            "Timed out waiting for range deletions on shard0 to complete",
        );

        // Use $queryStats with no app-name filter to catch any delete entry regardless of
        // which client issued it (internal or external).
        const entries = assert.commandWorked(
            st.shard0.adminCommand({
                aggregate: 1,
                pipeline: [
                    {$queryStats: {}},
                    {
                        $match: {
                            "key.queryShape.command": "delete",
                            "key.queryShape.cmdNs.coll": collName,
                        },
                    },
                ],
                cursor: {},
            }),
        ).cursor.firstBatch;

        assert.eq(entries.length, 0, "Range deletions should not appear in query stats", {entries});
    });

    // User-initiated writes targeting the donor shard that occur concurrently during chunk migration
    // are registered and collected in query stats.
    it("concurrent user writes on the migrating range are tracked in query stats on the donor", function () {
        const kDocs = [
            {_id: 1, v: 1},
            {_id: 2, v: 2},
            {_id: 3, v: 3},
            {_id: 4, v: 4},
            {_id: 5, v: 5},
            {_id: 6, v: 6},
        ];
        assert.commandWorked(coll.insert(kDocs));

        resetQueryStatsStore(st.shard0, "1MB");

        // Pause the migration after cloning but before the critical section. At this point the
        // donor still owns the chunk and accepts user writes.
        const fp = configureFailPoint(st.shard0, "moveChunkHangAtStep4");

        const awaitMigration = startParallelShell(
            funWithArgs(
                function (ns, toShard) {
                    assert.commandWorked(
                        db.adminCommand({moveChunk: ns, find: {_id: 1}, to: toShard}),
                    );
                },
                coll.getFullName(),
                st.shard1.shardName,
            ),
            st.s.port,
        );

        fp.wait();

        // Issue one of each write type through mongos while the migration is paused. Mongos
        // routes to shard0 because the chunk has not yet been committed to shard1.
        assert.commandWorked(testDB.runCommand({insert: collName, documents: [{_id: 7, v: 7}]}));
        assert.commandWorked(
            testDB.runCommand({delete: collName, deletes: [{q: {_id: 1}, limit: 1}]}),
        );
        assert.commandWorked(
            testDB.runCommand({update: collName, updates: [{q: {_id: 2}, u: {$set: {v: 99}}}]}),
        );

        fp.off();
        awaitMigration();

        // Writes should appear in shard0's query stats.
        const insertEntries = getQueryStatsInsertCmd(st.shard0, {collName});
        assert.eq(
            insertEntries.length,
            1,
            "Expected exactly 1 insert query stats entry on the donor shard",
            {insertEntries},
        );
        assert.eq(
            insertEntries[0].metrics.writes.nInserted.sum,
            1,
            "Expected exactly 1 insert recorded",
            {insertEntries},
        );

        const deleteEntries = getQueryStatsDeleteCmd(st.shard0, {collName});
        assert.eq(
            deleteEntries.length,
            1,
            "Expected exactly 1 delete query stats entry on the donor shard",
            {deleteEntries},
        );
        assert.eq(
            deleteEntries[0].metrics.writes.nDeleted.sum,
            1,
            "Expected exactly 1 delete recorded",
            {deleteEntries},
        );
        assert.docEq(
            deleteEntries[0].key.queryShape,
            {
                command: "delete",
                cmdNs: {db: testDB.getName(), coll: collName},
                q: {_id: {$eq: "?number"}},
                limit: 1,
            },
            "Expected query shape for user-issued single-doc delete by _id",
        );

        const updateEntries = getQueryStatsUpdateCmd(st.shard0, {collName});
        assert.eq(
            updateEntries.length,
            1,
            "Expected exactly 1 update query stats entry on the donor shard",
            {updateEntries},
        );
        assert.eq(
            updateEntries[0].metrics.writes.nModified.sum,
            1,
            "Expected exactly 1 update recorded",
            {updateEntries},
        );
        assert.docEq(
            updateEntries[0].key.queryShape,
            {
                command: "update",
                cmdNs: {db: testDB.getName(), coll: collName},
                q: {_id: {$eq: "?number"}},
                u: {$set: {v: "?number"}},
                multi: false,
                upsert: false,
            },
            "Expected query shape for user-issued $set update by _id",
        );
    });
});
