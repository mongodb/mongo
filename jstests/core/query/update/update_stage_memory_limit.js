/**
 * Test that verifies the behavior of UpdateStage when the memory limit for the record id
 * deduplicator (_updatedRecordIds) is exceeded during a multi-update.
 *
 * _updatedRecordIds is only allocated when isMulti() is true (to guard against the Halloween
 * problem), and a record id is only inserted into it when the update touches an indexed field
 * (indexesAffected=true). Both conditions must hold for the memory check to fire.
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
 *
 *   # A wildcard index covers all fields including 'y', making indexesAffected=true for
 *   # {$inc: {y: 1}} and causing the memory limit check to fire unexpectedly.
 *   wildcard_indexes_incompatible,
 * ]
 */

import {getPlanStage} from "jstests/libs/query/analyze_plan.js";
import {setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const coll = db.update_stage_memory_limit;
coll.drop();

const kDocCount = 1000;
const docs = Array.from({length: kDocCount}, (_, i) => ({_id: i, x: i, y: 0}));
assert.commandWorked(coll.insertMany(docs));

// Index on x. Updates that modify x (an indexed field) will set indexesAffected=true, which
// causes record ids to be inserted into _updatedRecordIds.
assert.commandWorked(coll.createIndex({x: 1}));

const kFilter = {x: {$gte: 0}};
const kUpdateIndexed = {$inc: {x: kDocCount}}; // modifies indexed field x
const kUpdateNoIndex = {$inc: {y: 1}}; // modifies non-indexed field y

// Check that the classic UPDATE stage is used. Skip if not.
const explainRes = assert.commandWorked(
    db.runCommand({
        explain: {update: coll.getName(), updates: [{q: kFilter, u: kUpdateIndexed, multi: true}]},
        verbosity: "queryPlanner",
    }),
);
if (getPlanStage(explainRes.queryPlanner.winningPlan, "UPDATE") === null) {
    jsTest.log.info("Skipping test: UPDATE stage not found. " + "This stage is only used by the classic engine.");
    quit();
}

// The multi-update/upsert should succeed with the default memory limit.
assert.commandWorked(coll.updateMany(kFilter, kUpdateIndexed));
assert.commandWorked(coll.updateMany(kFilter, kUpdateIndexed, {upsert: true}));

const originalLimit = assert.commandWorked(db.adminCommand({getParameter: 1, internalUpdateStageMaxMemoryBytes: 1}));

// A small limit forces the memory check to fail once enough record ids are inserted into
// the deduplicator.
try {
    setParameterOnAllNonConfigNodes(db.getMongo(), "internalUpdateStageMaxMemoryBytes", 1000);

    // Multi-update that modifies an indexed field (x): _updatedRecordIds grows, memory check
    // fires.
    assert.throwsWithCode(
        () => coll.updateMany(kFilter, kUpdateIndexed),
        [12227902],
        [] /*params*/,
        () => explainRes,
    );

    // Multi-update that does NOT modify any indexed field (y has no index): indexesAffected is
    // false so _updatedRecordIds stays empty and the query succeeds even with a small limit.
    assert.commandWorked(coll.updateMany(kFilter, kUpdateNoIndex));

    // Upsert with multi:true modifying an indexed field: same deduplication path, same failure.
    assert.throwsWithCode(
        () => coll.updateMany(kFilter, kUpdateIndexed, {upsert: true}),
        [12227902],
        [] /*params*/,
        () => explainRes,
    );
} finally {
    setParameterOnAllNonConfigNodes(
        db.getMongo(),
        "internalUpdateStageMaxMemoryBytes",
        originalLimit.internalUpdateStageMaxMemoryBytes,
    );
}
