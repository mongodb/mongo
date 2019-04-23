/**
 * Test distinct() on multikey indexes using a dotted path.
 *
 * Assumes the collection is not sharded, because sharding the collection could result in different
 * plans being chosen on different shards (for example, if an index is multikey on one shard but
 * not another).
 * Doesn't support stepdowns because it runs explain() on an aggregation (which can apparently
 * return incomplete results).
 * @tags: [assumes_unsharded_collection, does_not_support_stepdowns]
 */
(function() {
    "use strict";
    load("jstests/libs/analyze_plan.js");  // For planHasStage().

    const coll = db.distinct_multikey;
    coll.drop();
    assert.commandWorked(coll.createIndex({"a.b.c": 1}));

    assert.commandWorked(coll.insert({a: {b: {c: 1}}}));
    assert.commandWorked(coll.insert({a: {b: {c: 2}}}));
    assert.commandWorked(coll.insert({a: {b: {c: 3}}}));
    assert.commandWorked(coll.insert({a: {b: {notRelevant: 3}}}));
    assert.commandWorked(coll.insert({a: {notRelevant: 3}}));

    const numPredicate = {"a.b.c": {$gt: 0}};

    function getAggPipelineForDistinct(path) {
        return [{$group: {_id: "$" + path}}];
    }

    // Run an agg pipeline with a $group, and convert the results so they're equivalent
    // to what a distinct() would return.
    // Note that $group will treat an array as its own key rather than unwinding it. This means
    // that a $group on a field that's multikey will have different behavior than a distinct(), so
    // we only use this function for non-multikey fields.
    function distinctResultsFromPipeline(pipeline) {
        const res = coll.aggregate(pipeline).toArray();
        return res.map((x) => x._id);
    }

    // Be sure a distinct scan is used when the index is not multi key.
    (function testDistinctWithNonMultikeyIndex() {
        const results = coll.distinct("a.b.c");
        // TODO SERVER-14832: Returning 'null' here is inconsistent with the behavior when no index
        // is present.
        assert.sameMembers([1, 2, 3, null], results);

        const expl = coll.explain().distinct("a.b.c");
        assert.eq(true, planHasStage(db, expl.queryPlanner.winningPlan, "DISTINCT_SCAN"), expl);

        // Do an equivalent query using $group.
        const pipeline = getAggPipelineForDistinct("a.b.c");
        const aggResults = distinctResultsFromPipeline(pipeline);
        assert.sameMembers(aggResults, results);
        const aggExpl = assert.commandWorked(coll.explain().aggregate(pipeline));
        assert.gt(getAggPlanStages(aggExpl, "DISTINCT_SCAN").length, 0);
    })();

    // Distinct with a predicate.
    (function testDistinctWithPredWithNonMultikeyIndex() {
        const results = coll.distinct("a.b.c", numPredicate);
        assert.sameMembers([1, 2, 3], results);

        const expl = coll.explain().distinct("a.b.c", numPredicate);

        assert.eq(true, planHasStage(db, expl.queryPlanner.winningPlan, "DISTINCT_SCAN"), expl);

        const pipeline = [{$match: numPredicate}].concat(getAggPipelineForDistinct("a.b.c"));
        const aggResults = distinctResultsFromPipeline(pipeline);
        assert.sameMembers(aggResults, results);
        const aggExpl = assert.commandWorked(coll.explain().aggregate(pipeline));
        assert.gt(getAggPlanStages(aggExpl, "DISTINCT_SCAN").length, 0);
    })();

    // Make the index multi key.
    assert.commandWorked(coll.insert({a: {b: [{c: 4}, {c: 5}]}}));
    assert.commandWorked(coll.insert({a: {b: [{c: 4}, {c: 6}]}}));
    // Empty array is indexed as 'undefined'.
    assert.commandWorked(coll.insert({a: {b: {c: []}}}));

    // We should still use the index as long as the path we distinct() on is never an array
    // index.
    (function testDistinctWithMultikeyIndex() {
        const multiKeyResults = coll.distinct("a.b.c");
        // TODO SERVER-14832: Returning 'null' and 'undefined' here is inconsistent with the
        // behavior when no index is present.
        assert.sameMembers([1, 2, 3, 4, 5, 6, null, undefined], multiKeyResults);
        const expl = coll.explain().distinct("a.b.c");

        assert.eq(true, planHasStage(db, expl.queryPlanner.winningPlan, "DISTINCT_SCAN"));

        // Not running same query with $group now that the field is multikey. See comment above.
    })();

    // We cannot use the DISTINCT_SCAN optimization when there is a multikey path in the key and
    // there is a predicate. The reason is that we may have a predicate like {a: 4}, and two
    // documents: {a: [4, 5]}, {a: [4, 6]}. With a DISTINCT_SCAN, we would "skip over" one of the
    // documents, and leave out either '5' or '6', rather than providing the correct result of
    // [4, 5, 6]. The test below is for a similar case.
    (function testDistinctWithPredWithMultikeyIndex() {
        const pred = {"a.b.c": 4};
        const results = coll.distinct("a.b.c", pred);
        assert.sameMembers([4, 5, 6], results);

        const expl = coll.explain().distinct("a.b.c", pred);
        assert.eq(false, planHasStage(db, expl.queryPlanner.winningPlan, "DISTINCT_SCAN"), expl);
        assert.eq(true, planHasStage(db, expl.queryPlanner.winningPlan, "IXSCAN"), expl);

        // Not running same query with $group now that the field is multikey. See comment above.
    })();

    // Perform a distinct on a path where the last component is multikey.
    (function testDistinctOnPathWhereLastComponentIsMultiKey() {
        assert.commandWorked(coll.createIndex({"a.b": 1}));
        const multiKeyResults = coll.distinct("a.b");
        assert.sameMembers(
            [
              null,  // From the document with no 'b' field. TODO SERVER-14832: this is
                     // inconsistent with behavior when no index is present.
              {c: 1},
              {c: 2},
              {c: 3},
              {c: 4},
              {c: 5},
              {c: 6},
              {c: []},
              {notRelevant: 3}
            ],
            multiKeyResults);

        const expl = coll.explain().distinct("a.b");
        assert.eq(true, planHasStage(db, expl.queryPlanner.winningPlan, "DISTINCT_SCAN"));

        // Not running same query with $group now that the field is multikey. See comment above.
    })();

    (function testDistinctOnPathWhereLastComponentIsMultiKeyWithPredicate() {
        assert.commandWorked(coll.createIndex({"a.b": 1}));
        const pred = {"a.b": {$type: "array"}};
        const multiKeyResults = coll.distinct("a.b", pred);
        assert.sameMembers(
            [
              {c: 4},
              {c: 5},
              {c: 6},
            ],
            multiKeyResults);

        const expl = coll.explain().distinct("a.b", pred);
        assert.eq(false, planHasStage(db, expl.queryPlanner.winningPlan, "DISTINCT_SCAN"));
        assert.eq(true, planHasStage(db, expl.queryPlanner.winningPlan, "IXSCAN"));

        // Not running same query with $group now that the field is multikey. See comment above.
    })();

    // If the path we distinct() on includes an array index, a COLLSCAN should be used,
    // even if an index is available on the prefix to the array component ("a.b" in this case).
    (function testDistinctOnNumericMultikeyPathNoIndex() {
        const res = coll.distinct("a.b.0");
        assert.eq(res, [{c: 4}]);

        const expl = coll.explain().distinct("a.b.0");
        assert.eq(true, planHasStage(db, expl.queryPlanner.winningPlan, "COLLSCAN"), expl);

        // Will not attempt the equivalent query with aggregation, since $group by "a.b.0" will
        // only treat '0' as a field name (not array index).
    })();

    // Creating an index on "a.b.0" and doing a distinct on it should be able to use DISTINCT_SCAN.
    (function testDistinctOnNumericMultikeyPathWithIndex() {
        assert.commandWorked(coll.createIndex({"a.b.0": 1}));
        assert.commandWorked(coll.insert({a: {b: {0: "hello world"}}}));
        const res = coll.distinct("a.b.0");
        assert.sameMembers(res, [{c: 4}, "hello world"]);

        const expl = coll.explain().distinct("a.b.0");
        assert.eq(true, planHasStage(db, expl.queryPlanner.winningPlan, "DISTINCT_SCAN"), expl);

        // Will not attempt the equivalent query with aggregation, since $group by "a.b.0" will
        // only treat '0' as a field name (not array index).
    })();

    // Creating an index on "a.b.0" and doing a distinct on it should use an IXSCAN, as "a.b" is
    // multikey. See explanation above about why a DISTINCT_SCAN cannot be used when the path
    // given is multikey.
    (function testDistinctWithPredOnNumericMultikeyPathWithIndex() {
        const pred = {"a.b.0": {$type: "object"}};
        const res = coll.distinct("a.b.0", pred);
        assert.sameMembers(res, [{c: 4}]);

        const expl = coll.explain().distinct("a.b.0", pred);
        assert.eq(false, planHasStage(db, expl.queryPlanner.winningPlan, "DISTINCT_SCAN"), expl);
        assert.eq(true, planHasStage(db, expl.queryPlanner.winningPlan, "IXSCAN"), expl);

        // Will not attempt the equivalent query with aggregation, since $group by "a.b.0" will
        // only treat '0' as a field name (not array index).
    })();
})();
