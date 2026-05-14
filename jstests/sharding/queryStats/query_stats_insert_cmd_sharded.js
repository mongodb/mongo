/**
 * Tests that query stats are collected for `insert` commands that go through mongos on a sharded
 * cluster. Exercises the sharded write-path wiring tracked in SERVER-122076, follow-up to the
 * standalone / replica-set wiring landed in SERVER-122054.
 *
 * Invariants under test:
 *   - Exactly one `$queryStats` entry per `insert` command on the router, regardless of fan-out.
 *   - `execCount` increments once per top-level insert command (find-style invocation count),
 *     never per document and never per internal mongos retry.
 *   - The `queryShapeHash` recorded on the router equals the hash recorded on each participating
 *     shard, so cluster-wide aggregation collapses to one logical entry per shape.
 *   - StaleConfig retries internal to mongos do not double-count on either router or shard.
 *   - Retryable inserts (`lsid` + `txnNumber`) keep the one-command / one-execution invariant.
 *
 * @tags: [
 *   featureFlagQueryStatsInsert,
 *   requires_fcv_90,
 *   requires_sharding,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getLatestQueryStatsEntry,
    getQueryStats,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kQueryStatsServerParams = {
    internalQueryStatsRateLimit: -1,
    internalQueryStatsWriteCmdSampleRate: 1,
};

/**
 * Local helper — the standalone PR's `getQueryStatsInsertCmd` lives in `query_stats_utils.js` on
 * a parallel branch; until that helper lands here, filter by command shape directly. Mirrors the
 * structure of `getQueryStatsUpdateCmd`.
 */
function getInsertEntries(conn, collName) {
    return getQueryStats(conn, {
        extraMatch: {
            "key.queryShape.command": "insert",
            "key.queryShape.cmdNs.coll": collName,
        },
    });
}

describe("query stats insert command metrics (sharded)", function () {
    let st;
    let mongos;
    let testDB;

    before(function () {
        st = new ShardingTest({
            shards: 2,
            mongos: 1,
            mongosOptions: {setParameter: kQueryStatsServerParams},
            rsOptions: {setParameter: kQueryStatsServerParams},
        });
        mongos = st.s;
        testDB = mongos.getDB("test");
        assert.commandWorked(
            testDB.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}),
        );
    });

    after(function () {
        st?.stop();
    });

    beforeEach(function () {
        resetQueryStatsStore(mongos, "1MB");
        resetQueryStatsStore(st.shard0, "1MB");
        resetQueryStatsStore(st.shard1, "1MB");
    });

    // Unsharded collection on a sharded cluster: every insert lands on the primary shard, but the
    // command still flows through the router and must register a single router-side entry.
    describe("unsharded collection on sharded cluster", function () {
        const collName = jsTestName() + "_unsharded";
        let coll;

        before(function () {
            coll = testDB[collName];
        });

        beforeEach(function () {
            coll.drop();
        });

        it("should record a single-doc insert once on the router", function () {
            assert.commandWorked(testDB.runCommand({
                insert: collName,
                documents: [{v: 1}],
                comment: "unsharded single-doc",
            }));

            const entry = getLatestQueryStatsEntry(mongos, {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "insert");

            assertAggregatedMetricsSingleExec(entry, {
                keysExamined: 0,
                docsExamined: 0,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {nMatched: 0, nUpserted: 0, nModified: 0, nDeleted: 0, nInserted: 1, nUpdateOps: 0},
            });
            assertExpectedResults({
                results: entry,
                expectedQueryStatsKey: entry.key,
                expectedExecCount: 1,
                expectedDocsReturnedSum: 0,
                expectedDocsReturnedMax: 0,
                expectedDocsReturnedMin: 0,
                expectedDocsReturnedSumOfSq: 0,
            });
        });

        it("should record a multi-doc insert once on the router with summed nInserted", function () {
            assert.commandWorked(testDB.runCommand({
                insert: collName,
                documents: [{v: 1}, {v: 2}, {v: 3}],
                comment: "unsharded multi-doc",
            }));

            const entry = getLatestQueryStatsEntry(mongos, {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "insert");

            assertAggregatedMetricsSingleExec(entry, {
                keysExamined: 0,
                docsExamined: 0,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {nMatched: 0, nUpserted: 0, nModified: 0, nDeleted: 0, nInserted: 3, nUpdateOps: 0},
            });
            assertExpectedResults({
                results: entry,
                expectedQueryStatsKey: entry.key,
                expectedExecCount: 1,
                expectedDocsReturnedSum: 0,
                expectedDocsReturnedMax: 0,
                expectedDocsReturnedMin: 0,
                expectedDocsReturnedSumOfSq: 0,
            });
        });
    });

    // Sharded collection where every document in a batch targets a single shard.
    describe("sharded collection — single-shard target", function () {
        const collName = jsTestName() + "_single_shard";
        let coll;

        before(function () {
            coll = testDB[collName];
            // Shard on {_id: 1}; split at 0 and move the upper chunk to shard1.
            st.shardColl(coll, {_id: 1}, {_id: 0}, {_id: 0});
        });

        beforeEach(function () {
            assert.commandWorked(coll.deleteMany({}));
        });

        it("should record one entry for a batch that targets only shard0", function () {
            assert.commandWorked(testDB.runCommand({
                insert: collName,
                documents: [{_id: -1}, {_id: -2}, {_id: -3}],
                comment: "single-shard target shard0",
            }));

            const entries = getInsertEntries(mongos, collName);
            assert.eq(entries.length, 1, "Expected 1 mongos entry: " + tojson(entries));
            assert.eq(entries[0].metrics.execCount, 1);

            assertAggregatedMetricsSingleExec(entries[0], {
                keysExamined: 0,
                docsExamined: 0,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {nMatched: 0, nUpserted: 0, nModified: 0, nDeleted: 0, nInserted: 3, nUpdateOps: 0},
            });
        });

        it("should produce identical queryShapeHash on router and on the targeted shard", function () {
            assert.commandWorked(testDB.runCommand({
                insert: collName,
                documents: [{_id: -10}],
                comment: "shape-hash equality probe",
            }));

            const routerEntries = getInsertEntries(mongos, collName);
            const shard0Entries = getInsertEntries(st.shard0, collName);

            assert.eq(routerEntries.length, 1, "router entries: " + tojson(routerEntries));
            assert.eq(shard0Entries.length, 1, "shard0 entries: " + tojson(shard0Entries));

            // The queryShapeHash is the load-bearing identity for cross-cluster aggregation:
            // it must be identical on router and shard for `$queryStats` to collapse to one
            // logical entry per shape across the deployment.
            assert.eq(
                routerEntries[0].key.queryShape,
                shard0Entries[0].key.queryShape,
                "router and shard0 must register identical insert query shape",
            );
        });
    });

    // Sharded collection where one batch fans out to both shards. The router must record exactly
    // one entry with execCount=1 even though two shards each see an insert sub-batch.
    describe("sharded collection — multi-shard fan-out", function () {
        const collName = jsTestName() + "_fan_out";
        let coll;

        before(function () {
            coll = testDB[collName];
            st.shardColl(coll, {_id: 1}, {_id: 0}, {_id: 0});
        });

        beforeEach(function () {
            assert.commandWorked(coll.deleteMany({}));
        });

        it("should record one router-side entry with execCount=1 for a fan-out batch", function () {
            assert.commandWorked(testDB.runCommand({
                insert: collName,
                documents: [
                    {_id: -2},
                    {_id: -1},
                    {_id: 1},
                    {_id: 2},
                ],
                comment: "fan-out across two shards",
            }));

            const routerEntries = getInsertEntries(mongos, collName);
            assert.eq(routerEntries.length, 1, "Expected exactly 1 router entry: " + tojson(routerEntries));
            assert.eq(routerEntries[0].metrics.execCount, 1, "execCount must be 1 — one command, one invocation");

            assertAggregatedMetricsSingleExec(routerEntries[0], {
                keysExamined: 0,
                docsExamined: 0,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {nMatched: 0, nUpserted: 0, nModified: 0, nDeleted: 0, nInserted: 4, nUpdateOps: 0},
            });

            // Each shard sees its half of the batch as a single sub-insert, so each shard
            // records execCount=1 with nInserted=2.
            const shard0Entries = getInsertEntries(st.shard0, collName);
            const shard1Entries = getInsertEntries(st.shard1, collName);
            assert.eq(shard0Entries.length, 1, "shard0 entries: " + tojson(shard0Entries));
            assert.eq(shard1Entries.length, 1, "shard1 entries: " + tojson(shard1Entries));
            assert.eq(shard0Entries[0].metrics.execCount, 1);
            assert.eq(shard1Entries[0].metrics.execCount, 1);

            // All three sites agree on the shape — this is the cluster-wide aggregation guarantee.
            assert.eq(routerEntries[0].key.queryShape, shard0Entries[0].key.queryShape);
            assert.eq(routerEntries[0].key.queryShape, shard1Entries[0].key.queryShape);
        });
    });

    // StaleConfig retry: when a shard returns StaleConfig on the first attempt, mongos refreshes
    // routing and re-dispatches internally. The hook lives outside the retry loop, so the router
    // records exactly one execution.
    describe("StaleConfig retried insert", function () {
        const collName = jsTestName() + "_stale_config";
        let coll;
        let shard0Primary;

        before(function () {
            coll = testDB[collName];
            assert.commandWorked(testDB.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
            shard0Primary = st.rs0.getPrimary();
        });

        beforeEach(function () {
            assert.commandWorked(coll.deleteMany({}));
        });

        it("should record exactly one execution despite an internal StaleConfig retry", function () {
            // Drain pending range deletions so the {times: 1} failpoint fires against the
            // intended insert command, not a background task. Same pattern as the update test.
            assert.soon(
                () => shard0Primary.getDB("config").rangeDeletions.find().itcount() === 0,
                "Timed out waiting for range deletions on shard0 to complete",
            );

            const fp = configureFailPoint(shard0Primary, "alwaysThrowStaleConfigInfo", {}, {times: 1});

            const result = assert.commandWorked(testDB.runCommand({
                insert: collName,
                documents: [{_id: 1, v: 1}],
                comment: "StaleConfig retried insert",
            }));
            assert.eq(result.n, 1);
            assert.neq(coll.findOne({_id: 1}), null);

            assert(fp.waitWithTimeout(1000), "alwaysThrowStaleConfigInfo failpoint was never triggered");

            const routerEntries = getInsertEntries(mongos, collName);
            assert.eq(routerEntries.length, 1, "router entries: " + tojson(routerEntries));
            assert.eq(routerEntries[0].metrics.execCount, 1, "router execCount must not double-count");
            assert.eq(routerEntries[0].metrics.writes.nInserted.sum, 1);

            const shardEntries = getInsertEntries(st.shard0, collName);
            assert.eq(shardEntries.length, 1, "shard entries: " + tojson(shardEntries));
            assert.eq(shardEntries[0].metrics.execCount, 1, "shard execCount must not double-count");
            assert.eq(shardEntries[0].metrics.writes.nInserted.sum, 1);

            fp.off();
        });
    });

    // Retryable writes through mongos: the same one-command / one-execution invariant must hold,
    // and the router's record matches the find-style accounting of the standalone PR.
    describe("retryable inserts through mongos", function () {
        const collName = jsTestName() + "_retryable";
        let coll;

        before(function () {
            coll = testDB[collName];
            st.shardColl(coll, {_id: 1}, {_id: 0}, {_id: 0});
        });

        beforeEach(function () {
            assert.commandWorked(coll.deleteMany({}));
        });

        it("should record one execution per retryable insert command", function () {
            const lsid = {id: UUID()};
            const txnNumber = NumberLong(1);

            const firstCmd = {
                insert: collName,
                documents: [{_id: -1, v: 1}, {_id: 1, v: 2}],
                lsid: lsid,
                txnNumber: txnNumber,
                comment: "retryable insert first",
            };

            const firstResult = assert.commandWorked(testDB.runCommand(firstCmd));
            assert.eq(firstResult.n, 2);

            let entries = getInsertEntries(mongos, collName);
            assert.eq(entries.length, 1, "Expected 1 router entry after initial batch: " + tojson(entries));
            // execCount counts insert commands, not documents — one command = one execution.
            assert.eq(entries[0].metrics.execCount, 1);
            assert.eq(entries[0].metrics.writes.nInserted.sum, 2);

            // Retry the same lsid/txnNumber with one new statement appended. StmtIds 0 and 1
            // are already-executed retries; stmtId 2 is new. The router still treats this as
            // one command invocation → execCount bumps by 1; nInserted.sum reflects only the
            // statement that was actually executed.
            const retryCmd = {
                insert: collName,
                documents: [{_id: -1, v: 1}, {_id: 1, v: 2}, {_id: 2, v: 3}],
                lsid: lsid,
                txnNumber: txnNumber,
                comment: "retryable insert first", // identical comment → same shape
            };

            const retryResult = assert.commandWorked(testDB.runCommand(retryCmd));
            assert.eq(
                retryResult.retriedStmtIds,
                [0, 1],
                "Expected retriedStmtIds [0, 1]: " + tojson(retryResult.retriedStmtIds),
            );

            entries = getInsertEntries(mongos, collName);
            assert.eq(entries.length, 1, "Expected still 1 router entry after partial retry");
            assert.eq(
                entries[0].metrics.execCount,
                2,
                "execCount should be 2 — one per command, regardless of retry count",
            );
            assert.eq(
                entries[0].metrics.writes.nInserted.sum,
                3,
                "nInserted.sum should be 3 — only stmtId 2 was newly inserted on retry",
            );
        });
    });
});
