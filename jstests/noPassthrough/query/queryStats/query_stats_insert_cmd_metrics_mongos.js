/**
 * Tests that query stats are collected for insert commands routed through mongos and that execution
 * metrics from shards are aggregated into the router-side query stats entry.
 *
 * @tags: [featureFlagQueryStatsInsert]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    assertAggregatedMetricsSingleExec,
    assertExpectedResults,
    getLatestQueryStatsEntry,
    getQueryStatsInsertCmd,
    getQueryStatsUpdateCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("query stats insert command metrics (mongos)", function () {
    let st;
    let mongos;
    let testDB;

    before(function () {
        const queryStatsParams = {
            internalQueryStatsWriteCmdSampleRate: 1,
        };
        st = new ShardingTest({
            shards: 2,
            mongosOptions: {setParameter: queryStatsParams},
            rsOptions: {setParameter: queryStatsParams},
        });
        mongos = st.s;
        testDB = mongos.getDB("test");
        assert.commandWorked(
            testDB.adminCommand({
                enableSharding: testDB.getName(),
                primaryShard: st.shard1.shardName,
            }),
        );
    });

    after(function () {
        st?.stop();
    });

    beforeEach(function () {
        resetQueryStatsStore(mongos, "1MB");
    });

    // Verifies that mongos aggregates execution metrics from all shards when an insert fans out
    // to multiple shards. Documents with negative _id values go to shard1 (primary); non-negative to shard0 (non-primary).
    describe("multi-shard aggregation", function () {
        const collName = jsTestName() + "_multi_shard";
        let coll;

        before(function () {
            coll = testDB[collName];
            // shard1 is the primary (set in outer before). Shard on {_id: 1}, split at {_id: 0}:
            // negative _ids stay on shard1 (primary), non-negative move to shard0 (non-primary).
            assert.commandWorked(
                st.s.adminCommand({shardcollection: coll.getFullName(), key: {_id: 1}}),
            );
            assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
            assert.commandWorked(
                st.s.adminCommand({
                    movechunk: coll.getFullName(),
                    find: {_id: 0},
                    to: st.shard0.shardName,
                }),
            );
        });

        beforeEach(function () {
            assert.commandWorked(coll.deleteMany({}));
        });

        it("should aggregate nInserted from both shards when docs span multiple shards", function () {
            // Insert 3 docs to shard0 (non-negative _ids) and 2 docs to shard1 (negative _ids).
            const cmd = {
                insert: collName,
                documents: [
                    {_id: 1, v: "a"},
                    {_id: 2, v: "b"},
                    {_id: 3, v: "c"},
                    {_id: -1, v: "d"},
                    {_id: -2, v: "e"},
                ],
            };

            const result = assert.commandWorked(testDB.runCommand(cmd));
            assert.eq(result.n, 5);

            const entry = getLatestQueryStatsEntry(mongos, {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "insert");

            assertAggregatedMetricsSingleExec(entry, {
                keysExamined: 0,
                docsExamined: 0,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {
                    nMatched: 0,
                    nUpserted: 0,
                    nModified: 0,
                    nDeleted: 0,
                    nInserted: 5,
                    nUpdateOps: 0,
                    nDeleteOps: 0,
                },
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

        it("should record nInserted correctly for a single-shard insert", function () {
            // All docs have non-negative _id, so they all go to shard0 (non-primary).
            const cmd = {
                insert: collName,
                documents: [
                    {_id: 10, v: "x"},
                    {_id: 11, v: "y"},
                ],
            };

            const result = assert.commandWorked(testDB.runCommand(cmd));
            assert.eq(result.n, 2);

            const entry = getLatestQueryStatsEntry(mongos, {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "insert");

            assertAggregatedMetricsSingleExec(entry, {
                keysExamined: 0,
                docsExamined: 0,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {
                    nMatched: 0,
                    nUpserted: 0,
                    nModified: 0,
                    nDeleted: 0,
                    nInserted: 2,
                    nUpdateOps: 0,
                    nDeleteOps: 0,
                },
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

        it("should accumulate execCount and nInserted across multiple insert commands", function () {
            // Two separate insert commands with the same shape should accumulate into one entry.
            const cmd1 = {insert: collName, documents: [{_id: 20, v: 1}]};
            const cmd2 = {insert: collName, documents: [{_id: 21, v: 2}]};

            assert.commandWorked(testDB.runCommand(cmd1));
            assert.commandWorked(testDB.runCommand(cmd2));

            const entries = getQueryStatsInsertCmd(mongos, {collName: collName});
            assert.eq(entries.length, 1, "Expected 1 query stats entry: " + tojson(entries));
            assert.eq(entries[0].metrics.execCount, 2);
            assert.eq(
                entries[0].metrics.writes.nInserted.sum,
                NumberLong(2),
                "nInserted.sum should be 2 (one doc per command)",
            );
        });
    });

    // StaleConfig retry: when a shard returns StaleConfig, mongos retries the write internally.
    // Query stats should record the insert only once (for the successful execution).
    describe("StaleConfig retried insert", function () {
        const collName = jsTestName() + "_stale_config";
        let coll;
        let shard1Primary;

        before(function () {
            coll = testDB[collName];
            assert.commandWorked(
                testDB.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}),
            );
            shard1Primary = st.rs1.getPrimary();
        });

        it("should record query stats once despite StaleConfig retry", function () {
            resetQueryStatsStore(st.shard1, "1MB");

            // Wait for any pending range deletions on shard1 before activating the failpoint,
            // since alwaysThrowStaleConfigInfo fires for all namespaces.
            assert.soon(
                () => shard1Primary.getDB("config").rangeDeletions.find().itcount() === 0,
                "Timed out waiting for range deletions on shard1 to complete",
            );

            const fp = configureFailPoint(
                shard1Primary,
                "alwaysThrowStaleConfigInfo",
                {},
                {times: 1},
            );

            const result = assert.commandWorked(
                testDB.runCommand({
                    insert: collName,
                    documents: [{_id: 100, v: "stale_retry"}],
                }),
            );
            assert.eq(result.n, 1);

            assert(
                fp.waitWithTimeout(1000),
                "alwaysThrowStaleConfigInfo failpoint was never triggered",
            );

            const mongosEntries = getQueryStatsInsertCmd(mongos, {collName: collName});
            assert.eq(
                mongosEntries.length,
                1,
                "Expected 1 mongos query stats entry: " + tojson(mongosEntries),
            );

            const entry = getLatestQueryStatsEntry(mongos, {collName: coll.getName()});
            assert.eq(entry.key.queryShape.command, "insert");

            assertAggregatedMetricsSingleExec(entry, {
                keysExamined: 0,
                docsExamined: 0,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {
                    nMatched: 0,
                    nUpserted: 0,
                    nModified: 0,
                    nDeleted: 0,
                    nInserted: 1,
                    nUpdateOps: 0,
                    nDeleteOps: 0,
                },
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

            fp.off();
        });
    });

    // Verify parseAndRegisterRequest skips command shapes it does not handle:
    //   - bulkWrite / findAndModify: not a BatchedCommandRequest, so isBatchWriteCommand() is false.
    //   - delete: a BatchedCommandRequest, but BatchType_Delete is filtered out.
    // None of these should produce insert or update entries in mongos query stats.
    describe("skip unsupported command types", function () {
        const collName = jsTestName() + "_skip";
        let coll;

        before(function () {
            coll = testDB[collName];
            // shard1 is the primary (set in outer before). Non-negative _ids go to shard0 (non-primary).
            assert.commandWorked(
                st.s.adminCommand({shardcollection: coll.getFullName(), key: {_id: 1}}),
            );
            assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
            assert.commandWorked(
                st.s.adminCommand({
                    movechunk: coll.getFullName(),
                    find: {_id: 0},
                    to: st.shard0.shardName,
                }),
            );
        });

        it("should not record query stats for delete command", function () {
            assert.commandWorked(coll.insert({_id: 1, v: 1}));
            resetQueryStatsStore(mongos, "1MB");

            assert.commandWorked(
                testDB.runCommand({
                    delete: collName,
                    deletes: [{q: {_id: 1}, limit: 1}],
                }),
            );

            assert.eq(
                getQueryStatsInsertCmd(mongos, {collName: collName}).length,
                0,
                "Expected no insert query stats entry for delete command",
            );
            assert.eq(
                getQueryStatsUpdateCmd(mongos, {collName: collName}).length,
                0,
                "Expected no update query stats entry for delete command",
            );
        });

        it("should not record query stats for bulkWrite command", function () {
            resetQueryStatsStore(mongos, "1MB");

            assert.commandWorked(
                testDB.adminCommand({
                    bulkWrite: 1,
                    ops: [{insert: 0, document: {_id: 50, v: "bulk"}}],
                    nsInfo: [{ns: coll.getFullName()}],
                }),
            );

            assert.eq(
                getQueryStatsInsertCmd(mongos, {collName: collName}).length,
                0,
                "Expected no insert query stats entry for bulkWrite command",
            );
            assert.eq(
                getQueryStatsUpdateCmd(mongos, {collName: collName}).length,
                0,
                "Expected no update query stats entry for bulkWrite command",
            );
        });

        it("should not record query stats for findAndModify command", function () {
            assert.commandWorked(coll.insert({_id: 60, v: 1}));
            resetQueryStatsStore(mongos, "1MB");

            assert.commandWorked(
                testDB.runCommand({
                    findAndModify: collName,
                    query: {_id: 60},
                    update: {$set: {v: 999}},
                }),
            );

            assert.eq(
                getQueryStatsInsertCmd(mongos, {collName: collName}).length,
                0,
                "Expected no insert query stats entry for findAndModify command",
            );
            assert.eq(
                getQueryStatsUpdateCmd(mongos, {collName: collName}).length,
                0,
                "Expected no update query stats entry for findAndModify command",
            );
        });
    });

    // Timeseries inserts are handled by performTimeseriesWrites on the shard. Verify that
    // the shard-side execution metrics (nInserted) are still propagated back to mongos.
    describe("timeseries insert metrics", function () {
        const timeField = "time";
        const metaField = "meta";
        const collName = jsTestName() + "_ts";
        let coll;

        before(function () {
            coll = testDB[collName];
            // Collection is intentionally left unsharded so it lives entirely on shard1 (primary).
            assert.commandWorked(
                testDB.createCollection(collName, {
                    timeseries: {timeField: timeField, metaField: metaField},
                }),
            );
        });

        beforeEach(function () {
            resetQueryStatsStore(mongos, "1MB");
        });

        it("should record nInserted correctly for a timeseries insert routed through mongos", function () {
            const result = assert.commandWorked(
                testDB.runCommand({
                    insert: collName,
                    documents: [
                        {[timeField]: ISODate("2021-05-18T00:00:00.000Z"), v: 1, [metaField]: "a"},
                        {[timeField]: ISODate("2021-05-18T01:00:00.000Z"), v: 2, [metaField]: "a"},
                        {[timeField]: ISODate("2021-05-18T02:00:00.000Z"), v: 3, [metaField]: "b"},
                    ],
                }),
            );
            assert.eq(result.n, 3);

            const entry = getLatestQueryStatsEntry(mongos, {collName: collName});
            assert.eq(entry.key.queryShape.command, "insert");

            assertAggregatedMetricsSingleExec(entry, {
                keysExamined: 0,
                docsExamined: 0,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {
                    nMatched: 0,
                    nUpserted: 0,
                    nModified: 0,
                    nDeleted: 0,
                    nInserted: 3,
                    nUpdateOps: 0,
                    nDeleteOps: 0,
                },
            });
        });
    });
});
