/**
 * Tests that a {$**: 1} index can coexist with a {$**: 'text'} index in the same collection.
 *
 * Tagged as 'assumes_unsharded_collection' so that the expected relationship between the number of
 * IXSCANs in the explain output and the number of fields in the indexed documents is not distorted
 * by being spread across multiple shards.
 *
 * @tags: [assumes_unsharded_collection]
 *
 * TODO: SERVER-36198: Move this test back to jstests/core/
 */
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For arrayEq.
    load("jstests/libs/analyze_plan.js");         // For getPlanStages and planHasStage.

    const assertArrayEq = (l, r) => assert(arrayEq(l, r), tojson(l) + " != " + tojson(r));

    const coll = db.all_paths_and_text_indexes;
    coll.drop();

    // Runs a single allPaths query test, confirming that an indexed solution exists, that the $**
    // index on the given 'expectedPath' was used to answer the query, and that the results are
    // identical to those obtained via COLLSCAN.
    function assertAllPathsQuery(query, expectedPath) {
        // Explain the query, and determine whether an indexed solution is available.
        const explainOutput = coll.find(query).explain("executionStats");
        const ixScans = getPlanStages(explainOutput.queryPlanner.winningPlan, "IXSCAN");
        // Verify that the winning plan uses the $** index with the expected path.
        assert.eq(ixScans.length, 1);
        assert.docEq(ixScans[0].keyPattern, {"$_path": 1, [expectedPath]: 1});
        // Verify that the results obtained from the $** index are identical to a COLLSCAN.
        assertArrayEq(coll.find(query).toArray(), coll.find(query).hint({$natural: 1}).toArray());
    }

    // Insert documents containing the field '_fts', which is reserved when using a $text index.
    assert.commandWorked(coll.insert({_id: 1, a: 1, _fts: 1, textToSearch: "banana"}));
    assert.commandWorked(coll.insert({_id: 2, a: 1, _fts: 2, textToSearch: "bananas"}));
    assert.commandWorked(coll.insert({_id: 3, a: 1, _fts: 3}));

    // Required in order to build $** indexes.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryAllowAllPathsIndexes: true}));
    try {
        // Build an allPaths index, and verify that it can be used to query for the field '_fts'.
        assert.commandWorked(coll.createIndex({"$**": 1}));
        assertAllPathsQuery({_fts: {$gt: 0, $lt: 4}}, '_fts');

        // Perform the tests below for simple and compound $text indexes.
        for (let textIndex of[{'$**': 'text'}, {a: 1, '$**': 'text'}]) {
            // Build the appropriate text index.
            assert.commandWorked(coll.createIndex(textIndex, {name: "textIndex"}));

            // Confirm that the $** index can still be used to query for the '_fts' field outside of
            // a $text query.
            assertAllPathsQuery({_fts: {$gt: 0, $lt: 4}}, '_fts');

            // Confirm that $** does not generate a candidate plan for $text search, including cases
            // when the query filter contains a compound field in the $text index.
            const textQuery =
                Object.assign(textIndex.a ? {a: 1} : {}, {$text: {$search: 'banana'}});
            let explainOut = assert.commandWorked(coll.find(textQuery).explain("executionStats"));
            assert(planHasStage(coll.getDB(), explainOut.queryPlanner.winningPlan, "TEXT"));
            assert.eq(explainOut.queryPlanner.rejectedPlans.length, 0);
            assert.eq(explainOut.executionStats.nReturned, 2);

            // Confirm that $** does not generate a candidate plan for $text search, including cases
            // where the query filter contains a field which is not present in the text index.
            explainOut =
                assert.commandWorked(coll.find(Object.assign({_fts: {$gt: 0, $lt: 4}}, textQuery))
                                         .explain("executionStats"));
            assert(planHasStage(coll.getDB(), explainOut.queryPlanner.winningPlan, "TEXT"));
            assert.eq(explainOut.queryPlanner.rejectedPlans.length, 0);
            assert.eq(explainOut.executionStats.nReturned, 2);

            // Confirm that the $** index can be used alongside a $text predicate in an $or.
            explainOut = assert.commandWorked(
                coll.find({$or: [{_fts: 3}, textQuery]}).explain("executionStats"));
            assert.eq(explainOut.queryPlanner.rejectedPlans.length, 0);
            assert.eq(explainOut.executionStats.nReturned, 3);

            const textOrAllPaths = getPlanStages(explainOut.queryPlanner.winningPlan, "OR").shift();
            assert.eq(textOrAllPaths.inputStages.length, 2);
            const textBranch = (textOrAllPaths.inputStages[0].stage === "TEXT" ? 0 : 1);
            const allPathsBranch = (textBranch + 1) % 2;
            assert.eq(textOrAllPaths.inputStages[textBranch].stage, "TEXT");
            assert.eq(textOrAllPaths.inputStages[allPathsBranch].stage, "IXSCAN");
            assert.eq(textOrAllPaths.inputStages[allPathsBranch].keyPattern, {$_path: 1, _fts: 1});

            // Drop the index so that a different text index can be created.
            assert.commandWorked(coll.dropIndex("textIndex"));
        }
    } finally {
        // Disable $** indexes once the tests have either completed or failed.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryAllowAllPathsIndexes: false}));
        // TODO: SERVER-36444 remove this coll.drop because validation should work now.
        coll.drop();
    }
})();
