/**
 * Tests batch-deleting a large range of data.
 * This test does not rely on getMores on purpose, as this is a requirement for running on
 * tenant migration passthroughs.
 *
 * @tags: [
 *  does_not_support_retryable_writes,
 *  # TODO (SERVER-55909): make WUOW 'groupOplogEntries' the only mode of operation.
 *  does_not_support_transactions,
 *  multiversion_incompatible,
 *  no_selinux,
 *  requires_fcv_61,
 *  requires_non_retryable_writes,
 *  # TODO SERVER-87044: re-enable test in suites that perform random migrations
 *  assumes_balancer_off,
 * ]
 */

function populateAndMassDelete(queryPredicate) {
    const testDB = db.getSiblingDB('test');
    const coll = testDB['c'];

    const collCount = 94321;  // Intentionally not a multiple of batchedDeletesTargetBatchDocs.

    coll.drop();
    assert.commandWorked(
        coll.insertMany([...Array(collCount).keys()].map(x => ({_id: x, a: x})), {ordered: false}));

    assert.eq(collCount, coll.countDocuments({}));

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
    assert.eq(collCount, coll.countDocuments({}));
    assert.commandWorked(coll.deleteMany(queryPredicate));
    assert.eq(null, coll.findOne());
}

populateAndMassDelete({_id: {$gte: 0}});
populateAndMassDelete({a: {$gte: 0}});