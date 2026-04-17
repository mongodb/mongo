/**
 * Test that verifies the behavior of CountScan when the memory limit for the record id
 * duplicate tracker is exceeded.
 *
 * The memory check only fires when the index is multikey (duplicate removal enabled), because
 * a document with an array-valued indexed field can appear multiple times in the index.
 * With a non-multikey index, duplicate removal is disabled and record ids are never inserted
 * into the duplicate tracker, so the memory check is never reached.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   does_not_support_transactions,
 *   not_allowed_with_signed_security_token,
 *   requires_fcv_90,
 *   # setParameter may return different values after a failover.
 *   does_not_support_stepdowns,
 *   # setParameterOnAllNonConfigNodes requires a stable shard list.
 *   assumes_stable_shard_list,
 *   # COUNT_SCAN is not used for views; the memory limit check never fires.
 *   incompatible_with_views,
 *   # Timeseries collections do not support array values in indexed measurement fields.
 *   exclude_from_timeseries_crud_passthrough,
 * ]
 */

import {getPlanStage} from "jstests/libs/query/analyze_plan.js";
import {setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const coll = db.count_scan_memory_limit;
coll.drop();

const kDocCount = 1000;
// Each document has an array value for 'a', making the index on 'a' multikey so that
// duplicate removal is enabled in CountScan. Field 'b' is a scalar so its index is
// non-multikey.
const docs = Array.from({length: kDocCount}, (_, i) => ({_id: i, a: [i, i + kDocCount], b: i}));
assert.commandWorked(coll.insertMany(docs));

// Multikey index on 'a' (array field): duplicate removal enabled.
assert.commandWorked(coll.createIndex({a: 1}));
// Non-multikey index on 'b' (scalar field): duplicate removal disabled.
assert.commandWorked(coll.createIndex({b: 1}));

const kFilterA = {a: {$gte: 0}};
const kFilterB = {b: {$gte: 0}};

const explainRes = assert.commandWorked(
    db.runCommand({explain: {count: coll.getName(), query: kFilterA}, verbosity: "queryPlanner"}),
);
if (getPlanStage(explainRes.queryPlanner.winningPlan, "COUNT_SCAN") === null) {
    jsTest.log.info("Skipping test: COUNT_SCAN stage not found. " + "This stage is only used by the classic engine.");
    quit();
}

// Both counts should succeed with the default memory limit.
assert.eq(kDocCount, coll.find(kFilterA).count());
assert.eq(kDocCount, coll.find(kFilterB).count());

const originalLimit = assert.commandWorked(db.adminCommand({getParameter: 1, internalCountScanStageMaxMemoryBytes: 1}));

// A small limit forces the memory check to fail once enough record ids are inserted into
// the duplicate tracker.
try {
    setParameterOnAllNonConfigNodes(db.getMongo(), "internalCountScanStageMaxMemoryBytes", 1000);

    // Multikey index on 'a': duplicate removal is enabled, tracker grows, memory check fires.
    assert.throwsWithCode(
        () => coll.find(kFilterA).count(),
        [12227901],
        [] /*params*/,
        () => explainRes,
    );

    // Non-multikey index on 'b': duplicate removal is disabled.
    assert.eq(kDocCount, coll.find(kFilterB).count());
} finally {
    setParameterOnAllNonConfigNodes(
        db.getMongo(),
        "internalCountScanStageMaxMemoryBytes",
        originalLimit.internalCountScanStageMaxMemoryBytes,
    );
}
