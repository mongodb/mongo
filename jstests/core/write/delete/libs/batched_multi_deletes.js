/**
 * Tests batch-deleting a large range of data using a given predicate.
 * This test does not rely on getMores on purpose, as this is a requirement for running on
 * tenant migration passthroughs.
 */

export function runBatchedMultiDeletesTest(coll, queryPredicate) {
    const testDB = coll.getDB();

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
