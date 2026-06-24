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
                writes: {
                    nMatched: 0,
                    nUpserted: 0,
                    nModified: 0,
                    nDeleted: 0,
                    nInserted: 0,
                    nUpdateOps: 1,
                    nDeleteOps: 0,
                },
            });
        });
    });
}

// Verifies that mongos sums per-shard keysInserted/keysDeleted for an update that maintains a
// secondary index and fans out to multiple shards. The other mongos update scenarios only touch
// non-indexed fields (so they report 0/0); this is the case that proves the index-key counts are
// actually aggregated across shards without double-counting or under-counting.
function IndexedFieldUpdateFanoutTests(ctxFn) {
    describe("indexed-field update fanout", function () {
        const collName = "update_indexed_fanout";
        const docsPerShard = 3;
        const totalDocs = docsPerShard * 2;
        let coll;

        before(function () {
            const {st, testDB} = ctxFn();
            coll = testDB[collName];
            // Shard on {_id: 1}, split at 0, and move the upper chunk so negative _ids live on one
            // shard and non-negative on the other; a multi:true update with no shard-key filter then
            // fans out to both shards. The {w: 1} secondary index is what gets maintained below.
            st.shardColl(coll, {_id: 1}, {_id: 0}, {_id: 0});
            assert.commandWorked(coll.createIndex({w: 1}));
        });

        beforeEach(function () {
            const {mongos} = ctxFn();
            assert.commandWorked(coll.deleteMany({}));
            const docs = [];
            for (let i = 0; i < docsPerShard; i++) docs.push({_id: -(i + 1), w: i + 1});
            for (let i = 0; i < docsPerShard; i++) docs.push({_id: i + 1, w: docsPerShard + i + 1});
            assert.commandWorked(coll.insertMany(docs));
            resetQueryStatsStore(mongos, "1MB");
        });

        it("sums per-shard keysInserted/keysDeleted across shards", function () {
            const {mongos, testDB} = ctxFn();
            // multi:true update of the indexed field 'w' with no shard-key filter -> fans out to both
            // shards. Every document is modified (none start with w: 9999), and on each one the old
            // {w} index key is deleted and the new key inserted.
            const result = assert.commandWorked(
                testDB.runCommand({
                    update: collName,
                    updates: [{q: {}, u: {$set: {w: 9999}}, multi: true}],
                }),
            );
            assert.eq(result.nModified, totalDocs, result);

            const entry = getLatestQueryStatsEntry(mongos, {collName});
            assert.eq(entry.key.queryShape.command, "update");
            assertAggregatedMetricsSingleExec(entry, {
                // The empty filter is satisfied by a collection scan on each shard, so no index keys
                // are examined to find the documents.
                keysExamined: 0,
                docsExamined: totalDocs,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
                writes: {
                    nMatched: totalDocs,
                    nUpserted: 0,
                    nModified: totalDocs,
                    nDeleted: 0,
                    nInserted: 0,
                    nUpdateOps: 1,
                    nDeleteOps: 0,
                    // {w: 1} is the only non-_id index, so updating w on every document deletes one
                    // old key and inserts one new key per document. mongos sums these counts from both
                    // shards, so each equals the total number of modified documents.
                    keysInserted: totalDocs,
                    keysDeleted: totalDocs,
                },
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
        nDeleteOps: 0,
        // These updates only set non-indexed fields, so no index keys are added or removed; the
        // keysPerDoc argument is intentionally ignored here. The indexed-field fanout case (where it
        // matters) is covered by IndexedFieldUpdateFanoutTests below.
        keysInserted: 0,
        keysDeleted: 0,
    }),
    validateCmdFn: (result) => assert.eq(result.nModified, 1, result),
    getQueryStatsFn: getQueryStatsUpdateCmd,
    extraTests: (ctxFn) => {
        WouldChangeOwningShardTests(ctxFn);
        IndexedFieldUpdateFanoutTests(ctxFn);
    },
});
