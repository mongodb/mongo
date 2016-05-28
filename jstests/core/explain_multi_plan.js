/**
 * Tests running explain on a variety of explainable commands (find, update, remove, etc.) when
 * there are multiple plans available. This is a regression test for SERVER-20849 and SERVER-21376.
 */
(function() {
    "use strict";
    var coll = db.explainMultiPlan;
    coll.drop();

    // Create indices to ensure there are multiple plans available.
    assert.commandWorked(coll.ensureIndex({a: 1, b: 1}));
    assert.commandWorked(coll.ensureIndex({a: 1, b: -1}));

    // Insert some data to work with.
    var bulk = coll.initializeOrderedBulkOp();
    var nDocs = 100;
    for (var i = 0; i < nDocs; ++i) {
        bulk.insert({a: i, b: nDocs - i});
    }
    bulk.execute();

    // SERVER-20849: The following commands should not crash the server.
    assert.doesNotThrow(function() {
        coll.explain("allPlansExecution").update({a: {$gte: 1}}, {$set: {x: 0}});
    });

    assert.doesNotThrow(function() {
        coll.explain("allPlansExecution").remove({a: {$gte: 1}});
    });

    assert.doesNotThrow(function() {
        coll.explain("allPlansExecution").findAndModify({query: {a: {$gte: 1}}, remove: true});
    });

    assert.doesNotThrow(function() {
        coll.explain("allPlansExecution").findAndModify({query: {a: {$gte: 1}}, update: true});
    });

    assert.doesNotThrow(function() {
        coll.explain("allPlansExecution").find({a: {$gte: 1}}).finish();
    });

    assert.doesNotThrow(function() {
        coll.explain("allPlansExecution").count({a: {$gte: 1}});
    });

    assert.doesNotThrow(function() {
        coll.explain("allPlansExecution").distinct("a", {a: {$gte: 1}});
    });

    assert.doesNotThrow(function() {
        coll.explain("allPlansExecution").group({
            key: {a: 1},
            cond: {a: {$gte: 1}},
            reduce: function(curr, result) {},
            initial: {}
        });
    });

    // SERVER-21376: Make sure the 'rejectedPlans' field is filled in appropriately.
    function assertHasRejectedPlans(explainOutput) {
        var queryPlannerOutput = explainOutput.queryPlanner;

        // The 'rejectedPlans' section will be in a different place if passed through a mongos.
        if ("SINGLE_SHARD" == queryPlannerOutput.winningPlan.stage) {
            var shards = queryPlannerOutput.winningPlan.shards;
            shards.forEach(function assertShardHasRejectedPlans(shard) {
                assert.gt(shard.rejectedPlans.length, 0);
            });
        } else {
            assert.gt(queryPlannerOutput.rejectedPlans.length, 0);
        }
    }

    var res = coll.explain("queryPlanner").find({a: {$gte: 1}}).finish();
    assert.commandWorked(res);
    assertHasRejectedPlans(res);

    res = coll.explain("executionStats").find({a: {$gte: 1}}).finish();
    assert.commandWorked(res);
    assertHasRejectedPlans(res);
}());
