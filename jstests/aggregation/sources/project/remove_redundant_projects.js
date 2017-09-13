// Tests that the aggregation pipeline correctly coalesces a $project stage at the front of the
// pipeline that can be covered by a normal query.
// @tags: [do_not_wrap_aggregations_in_facets]
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For orderedArrayEq.
    load('jstests/libs/analyze_plan.js');         // For planHasStage().

    let coll = db.remove_redundant_projects;
    coll.drop();

    assert.writeOK(coll.insert({_id: {a: 1, b: 1}, a: 1, c: {d: 1}, e: ['elem1']}));

    let indexSpec = {a: 1, 'c.d': 1, 'e.0': 1};

    /**
     * Helper to test that for a given pipeline, the same results are returned whether or not an
     * index is present.  Also tests whether a projection is absorbed by the pipeline
     * ('expectProjectToCoalesce') and the corresponding project stage ('removedProjectStage') does
     * not exist in the explain output.
     */
    function assertResultsMatch(
        pipeline, expectProjectToCoalesce, removedProjectStage = null, index = indexSpec) {
        // Add a match stage to ensure index scans are considered for planning (workaround for
        // SERVER-20066).
        pipeline = [{$match: {a: {$gte: 0}}}].concat(pipeline);

        // Once with an index.
        assert.commandWorked(coll.createIndex(index));
        let explain = coll.explain().aggregate(pipeline);
        let resultsWithIndex = coll.aggregate(pipeline).toArray();

        // Projection does not get pushed down when sharding filter is used.
        if (!explain.hasOwnProperty("shards")) {
            // Check that $project uses the query system.
            assert.eq(
                expectProjectToCoalesce,
                planHasStage(explain.stages[0].$cursor.queryPlanner.winningPlan, "PROJECTION"));

            // Check that $project was removed from pipeline and pushed to the query system.
            explain.stages.forEach(function(stage) {
                if (stage.hasOwnProperty("$project"))
                    assert.neq(removedProjectStage, stage["$project"]);
            });
        }

        // Again without an index.
        assert.commandWorked(coll.dropIndex(index));
        let resultsWithoutIndex = coll.aggregate(pipeline).toArray();

        assert(orderedArrayEq(resultsWithIndex, resultsWithoutIndex));
    }

    // Test that covered projections correctly use the query system for projection and the $project
    // stage is removed from the pipeline.
    assertResultsMatch([{$project: {_id: 0, a: 1}}], true, {_id: 0, a: 1});
    assertResultsMatch(
        [{$project: {_id: 0, a: 1}}, {$group: {_id: null, a: {$sum: "$a"}}}], true, {_id: 0, a: 1});
    assertResultsMatch([{$sort: {a: -1}}, {$project: {_id: 0, a: 1}}], true, {_id: 0, a: 1});
    assertResultsMatch(
        [
          {$sort: {a: 1, 'c.d': 1}},
          {$project: {_id: 0, a: 1}},
          {$group: {_id: "$a", arr: {$push: "$a"}}}
        ],
        true,
        {_id: 0, a: 1});
    assertResultsMatch([{$project: {_id: 0, c: {d: 1}}}], true, {_id: 0, c: {d: 1}});

    // Test that projections with renamed fields are not removed from the pipeline, however an
    // inclusion projection is still pushed to the query system.
    assertResultsMatch([{$project: {_id: 0, f: "$a"}}], true);
    assertResultsMatch([{$project: {_id: 0, a: 1, f: "$a"}}], true);

    // Test that uncovered projections include the $project stage in the pipeline.
    assertResultsMatch([{$project: {_id: 1, a: 1}}], false);
    assertResultsMatch([{$project: {_id: 0, a: 1, b: 1}}], false);
    assertResultsMatch([{$sort: {a: 1}}, {$project: {_id: 1, b: 1}}], false);
    assertResultsMatch(
        [{$sort: {a: 1}}, {$group: {_id: "$_id", arr: {$push: "$a"}}}, {$project: {arr: 1}}],
        false);

    // Test that projections with computed fields are kept in the pipeline.
    assertResultsMatch([{$project: {computedField: {$sum: "$a"}}}], false);
    assertResultsMatch([{$project: {a: ["$a", "$b"]}}], false);
    assertResultsMatch(
        [{$project: {e: {$filter: {input: "$e", as: "item", cond: {"$eq": ["$$item", "elem0"]}}}}}],
        false);

    // Test that only the first projection is removed from the pipeline.
    assertResultsMatch(
        [
          {$project: {_id: 0, a: 1}},
          {$group: {_id: "$a", arr: {$push: "$a"}, a: {$sum: "$a"}}},
          {$project: {_id: 0}}
        ],
        true,
        {_id: 0, a: 1});

    // Test that projections on _id with nested fields are not removed from pipeline. Due to
    // SERVER-7502, the dependency analysis does not generate a covered projection for nested
    // fields in _id and thus we cannot remove the stage.
    indexSpec = {'_id.a': 1, a: 1};
    assertResultsMatch([{$project: {'_id.a': 1}}], false, null, indexSpec);

}());
