/**
 * Tests query stats for update commands that go through various mongos dispatch paths.
 * @tags: [requires_fcv_90]
 */
import {before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    assertAggregatedMetricsSingleExec,
    getLatestQueryStatsEntry,
    getQueryStatsUpdateCmd,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";
import {runMongosWriteMetricsTests} from "jstests/libs/query/query_stats_write_cmd_utils.js";

const testCommands = {
    multiAll: (coll) => ({
        update: coll,
        updates: [{q: {}, u: {$set: {updated: true}}, multi: true}],
    }),
    multiTargeted: (coll) => ({
        update: coll,
        updates: [{q: {_id: {$gt: 0}}, u: {$set: {updated: true}}, multi: true}],
    }),
    singleOp: (coll, filterValue) => ({
        update: coll,
        updates: [{q: {filterField: filterValue}, u: {$set: {updated: true}}, multi: false}],
    }),
    noMatch: (coll) => ({
        update: coll,
        updates: [{q: {filterField: "nonexistent"}, u: {$set: {updated: true}}, multi: false}],
    }),
    batchOp: (coll, f1, f2) => ({
        update: coll,
        updates: [
            {q: {filterField: f1}, u: {$set: {updated: true}}, multi: false},
            {q: {filterField: f2}, u: {$set: {updated: true}}, multi: false},
        ],
    }),
    byId: (coll) => ({
        update: coll,
        updates: [{q: {_id: 1}, u: {$set: {v: 100}}, multi: false}],
    }),
};

function WouldChangeOwningShardTests(ctxFn) {
    // TODO SERVER-121267: enable when WCOS query stats propagation is fixed.
    describe.skip("update that triggers WouldChangeOwningShard", function () {
        const collName = "update_wcos";
        let coll;

        before(function () {
            const {st, testDB} = ctxFn();
            coll = testDB[collName];
            st.shardColl(coll, {sk: 1}, {sk: 0}, {sk: 0});
            for (const rs of [st.rs0, st.rs1]) {
                assert.soon(
                    () => rs.getPrimary().getDB("config").rangeDeletions.find().itcount() === 0,
                    "Timed out waiting for range deletions to complete on " + rs.name,
                );
            }
        });

        beforeEach(function () {
            const {st} = ctxFn();
            assert.commandWorked(coll.deleteMany({}));
            assert.commandWorked(
                coll.insertMany([
                    {_id: 1, sk: -1, v: 1},
                    {_id: 2, sk: 1, v: 2},
                ]),
            );
            resetQueryStatsStore(st, "1MB");
        });

        it("should collect query stats when update changes shard key ownership", function () {
            const {testDB} = ctxFn();
            const result = assert.commandWorked(
                testDB.runCommand({
                    update: collName,
                    updates: [{q: {sk: -1}, u: {$set: {sk: 5}}, multi: false}],
                    lsid: {id: UUID()},
                    txnNumber: NumberLong(1),
                }),
            );
            assert.eq(result.nModified, 1);
            assert.eq(coll.findOne({_id: 1}).sk, 5);

            const entry = getLatestQueryStatsEntry(testDB.getMongo(), {collName});
            assert.eq(entry.key.queryShape.command, "update");
            // TODO SERVER-121267: all metrics are 0 due to WCOS propagation gap.
            assertAggregatedMetricsSingleExec(entry, {
                keysExamined: 0,
                docsExamined: 0,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {nMatched: 0, nUpserted: 0, nModified: 0, nDeleted: 0, nInserted: 0, nUpdateOps: 1},
            });
        });
    });
}

runMongosWriteMetricsTests({
    label: "update",
    commands: testCommands,
    validateWriteMetricsFn: (n) => ({
        nMatched: n,
        nUpserted: 0,
        nModified: n,
        nDeleted: 0,
        nInserted: 0,
        nUpdateOps: 1,
    }),
    validateCmdFn: (result) => assert.eq(result.nModified, 1, result),
    getQueryStatsFn: getQueryStatsUpdateCmd,
    extraTests: (ctxFn) => {
        WouldChangeOwningShardTests(ctxFn);
    },
});
