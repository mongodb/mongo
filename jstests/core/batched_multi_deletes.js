/**
 * Tests batch-deleting a large range of data.
 *
 * @tags: [
 *  # TODO (SERVER-66071): support sharded collections
 *  assumes_unsharded_collection,
 *  does_not_support_retryable_writes,
 *  # TODO (SERVER-55909): make WUOW 'groupOplogEntries' the only mode of operation.
 *  does_not_support_transactions,
 *  featureFlagBatchMultiDeletes,
 *  multiversion_incompatible,
 *  no_selinux,
 *  requires_fcv_60,
 *  requires_getmore,
 *  requires_non_retryable_writes,
 * ]
 */

(function() {
"use strict";

function populateAndMassDelete(queryPredicate) {
    const testDB = db.getSiblingDB('test');
    const coll = testDB['c'];

    const collCount =
        54321;  // Intentionally not a multiple of BatchedDeleteStageBatchParams::targetBatchDocs.

    coll.drop();
    assert.commandWorked(coll.insertMany([...Array(collCount).keys()].map(x => ({_id: x, a: x}))));

    assert.eq(collCount, coll.find().itcount());

    // Verify the delete will involve the BATCHED_DELETE stage.
    const expl = testDB.runCommand({
        explain: {delete: coll.getName(), deletes: [{q: queryPredicate, limit: 0}]},
        verbosity: "executionStats"
    });
    assert.commandWorked(expl);

    if (expl["queryPlanner"]["winningPlan"]["stage"] === "SHARD_WRITE") {
        // This is a sharded cluster. Verify all shards execute the BATCHED_DELETE stage.
        for (let shard of expl["queryPlanner"]["winningPlan"]["shards"]) {
            assert.eq(shard["winningPlan"]["stage"], "BATCHED_DELETE");
        }
    } else {
        // Non-sharded
        assert.eq(expl["queryPlanner"]["winningPlan"]["stage"], "BATCHED_DELETE");
    }

    // Execute and verify the deletion.
    assert.eq(collCount, coll.find().itcount());
    assert.commandWorked(coll.deleteMany(queryPredicate));
    assert.eq(0, coll.find().itcount());
}

populateAndMassDelete({_id: {$gte: 0}});
populateAndMassDelete({a: {$gte: 0}});
})();
