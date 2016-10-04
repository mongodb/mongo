/**
 * SERVER-12015 Re-enable covered indices in the aggregation pipeline.
 *
 * This test executes some simple pipelines and asserts that they have the same results whether or
 * not there are indices that could cover the projection part of the query, or provide a
 * non-blocking sort.
 */

load("jstests/aggregation/extras/utils.js");  // For orderedArrayEq.

(function() {
    "use strict";
    var coll = db.server12015;
    coll.drop();
    var indexSpec = {a: 1, b: 1};

    assert.writeOK(coll.insert({_id: 0, a: 0, b: 0}));
    assert.writeOK(coll.insert({_id: 1, a: 0, b: 1}));
    assert.writeOK(coll.insert({_id: 2, a: 1, b: 0}));
    assert.writeOK(coll.insert({_id: 3, a: 1, b: 1}));

    /**
     * Helper to test that for a given pipeline, the same results are returned whether or not an
     * index is present.
     */
    function assertResultsMatch(pipeline) {
        // Add a match stage to ensure index scans are considerd for planning (workaround for
        // SERVER-20066).
        pipeline = [{$match: {a: {$gte: 0}}}].concat(pipeline);

        // Once with an index.
        assert.commandWorked(coll.ensureIndex(indexSpec));
        var resultsWithIndex = coll.aggregate(pipeline).toArray();

        // Again without an index.
        assert.commandWorked(coll.dropIndex(indexSpec));
        var resultsWithoutIndex = coll.aggregate(pipeline).toArray();

        assert(orderedArrayEq(resultsWithIndex, resultsWithoutIndex));
    }

    // Uncovered $project, no $sort.
    assertResultsMatch([{$project: {_id: 1, a: 1, b: 1}}]);

    // Covered $project, no $sort.
    assertResultsMatch([{$project: {_id: 0, a: 1}}]);
    assertResultsMatch([{$project: {_id: 0, a: 1, b: 1}}]);
    assertResultsMatch([{$project: {_id: 0, a: 1, b: 1, c: {$literal: 1}}}]);
    assertResultsMatch([{$project: {_id: 0, a: 1, b: 1}}, {$project: {a: 1}}]);
    assertResultsMatch([{$project: {_id: 0, a: 1, b: 1}}, {$group: {_id: null, a: {$sum: "$a"}}}]);

    // Non-blocking $sort, uncovered $project.
    assertResultsMatch([{$sort: {a: -1, b: -1}}, {$project: {_id: 1, a: 1, b: 1}}]);
    assertResultsMatch([{$sort: {a: 1, b: 1}}, {$project: {_id: 1, a: 1, b: 1}}]);
    assertResultsMatch(
        [{$sort: {a: 1, b: 1}}, {$group: {_id: "$_id", arr: {$push: "$a"}, sum: {$sum: "$b"}}}]);

    // Non-blocking $sort, covered $project.
    assertResultsMatch([{$sort: {a: -1, b: -1}}, {$project: {_id: 0, a: 1, b: 1}}]);
    assertResultsMatch([{$sort: {a: 1, b: 1}}, {$project: {_id: 0, a: 1, b: 1}}]);
    assertResultsMatch([{$sort: {a: 1, b: 1}}, {$group: {_id: "$b", arr: {$push: "$a"}}}]);

    // Blocking $sort, uncovered $project.
    assertResultsMatch([{$sort: {b: 1, a: -1}}, {$project: {_id: 1, a: 1, b: 1}}]);
    assertResultsMatch(
        [{$sort: {b: 1, a: -1}}, {$group: {_id: "$_id", arr: {$push: "$a"}, sum: {$sum: "$b"}}}]);

    // Blocking $sort, covered $project.
    assertResultsMatch([{$sort: {b: 1, a: -1}}, {$project: {_id: 0, a: 1, b: 1}}]);
    assertResultsMatch([{$sort: {b: 1, a: -1}}, {$group: {_id: "$b", arr: {$push: "$a"}}}]);
}());
