// Confirms expected index use when performing a match with a $expr statement.

(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");

    const isMMAPv1 = jsTest.options().storageEngine === "mmapv1";

    const coll = db.expr_index_use;
    coll.drop();

    assert.writeOK(coll.insert({a: {b: 1}}));
    assert.writeOK(coll.insert({a: {b: [1]}}));
    assert.writeOK(coll.insert({a: [{b: 1}]}));
    assert.writeOK(coll.insert({a: [{b: [1]}]}));
    assert.commandWorked(coll.createIndex({"a.b": 1}));

    assert.writeOK(coll.insert({c: {d: 1}}));
    assert.commandWorked(coll.createIndex({"c.d": 1}));

    assert.writeOK(coll.insert({e: [{f: 1}]}));
    assert.commandWorked(coll.createIndex({"e.f": 1}));

    assert.writeOK(coll.insert({g: {h: [1]}}));
    assert.commandWorked(coll.createIndex({"g.h": 1}));

    assert.writeOK(coll.insert({i: 1, j: [1]}));
    assert.commandWorked(coll.createIndex({i: 1, j: 1}));

    assert.writeOK(coll.insert({k: 1, l: "abc"}));
    assert.commandWorked(coll.createIndex({k: 1, l: "text"}));

    assert.writeOK(coll.insert({x: 0}));
    assert.writeOK(coll.insert({x: 1, y: 1}));
    assert.writeOK(coll.insert({x: 2, y: 2}));
    assert.writeOK(coll.insert({x: 3, y: 10}));
    assert.writeOK(coll.insert({y: 20}));
    assert.commandWorked(coll.createIndex({x: 1, y: 1}));

    assert.writeOK(coll.insert({w: 123}));
    assert.writeOK(coll.insert({}));
    assert.writeOK(coll.insert({w: null}));
    assert.writeOK(coll.insert({w: undefined}));
    assert.writeOK(coll.insert({w: NaN}));
    assert.writeOK(coll.insert({w: "foo"}));
    assert.writeOK(coll.insert({w: "FOO"}));
    assert.writeOK(coll.insert({w: {z: 1}}));
    assert.writeOK(coll.insert({w: {z: 2}}));
    assert.commandWorked(coll.createIndex({w: 1}));
    assert.commandWorked(coll.createIndex({"w.z": 1}));

    /**
     * Executes the expression 'expr' as both a find and an aggregate. Then confirms
     * 'metricsToCheck', which is an object containing:
     *  - nReturned:        The number of documents the pipeline is expected to return.
     *  - expectedIndex:    Either an index specification object when index use is expected or
     *                      'null' if a collection scan is expected.
     */
    function confirmExpectedExprExecution(expr, metricsToCheck, collation) {
        assert(metricsToCheck.hasOwnProperty("nReturned"),
               "metricsToCheck must contain an nReturned field");

        let aggOptions = {};
        if (collation) {
            aggOptions.collation = collation;
        }

        const pipeline = [{$match: {$expr: expr}}];

        // Verify that $expr returns the correct number of results when run inside the $match stage
        // of an aggregate.
        assert.eq(metricsToCheck.nReturned, coll.aggregate(pipeline, aggOptions).itcount());

        // Verify that $expr returns the correct number of results when run in a find command.
        let cursor = coll.find({$expr: expr});
        if (collation) {
            cursor = cursor.collation(collation);
        }
        assert.eq(metricsToCheck.nReturned, cursor.itcount());

        // Verify that $expr returns the correct number of results when evaluated inside a $project,
        // with optimizations inhibited. We expect the plan to be COLLSCAN.
        const pipelineWithProject = [
            {$_internalInhibitOptimization: {}},
            {$project: {result: {$cond: [expr, true, false]}}},
            {$match: {result: true}}
        ];
        assert.eq(metricsToCheck.nReturned,
                  coll.aggregate(pipelineWithProject, aggOptions).itcount());
        let explain = coll.explain("executionStats").aggregate(pipelineWithProject, aggOptions);
        assert(getAggPlanStage(explain, "COLLSCAN"), tojson(explain));

        // Verifies that there are no rejected plans, and that the winning plan uses the expected
        // index.
        //
        // 'getPlanStageFunc' is a function which can be called to obtain stage-specific information
        // from the explain output. There are different versions of this function for find and
        // aggregate explain output.
        function verifyExplainOutput(explain, getPlanStageFunc) {
            assert(!hasRejectedPlans(explain), tojson(explain));

            if (metricsToCheck.hasOwnProperty("expectedIndex")) {
                const stage = getPlanStageFunc(explain, "IXSCAN");
                assert.neq(null, stage, tojson(explain));
                assert(stage.hasOwnProperty("keyPattern"), tojson(explain));
                assert.docEq(stage.keyPattern, metricsToCheck.expectedIndex, tojson(explain));
            } else {
                assert(getPlanStageFunc(explain, "COLLSCAN"), tojson(explain));
            }
        }

        explain =
            assert.commandWorked(coll.explain("executionStats").aggregate(pipeline, aggOptions));
        verifyExplainOutput(explain, getAggPlanStage);

        cursor = coll.explain("executionStats").find({$expr: expr});
        if (collation) {
            cursor = cursor.collation(collation);
        }
        explain = assert.commandWorked(cursor.finish());
        verifyExplainOutput(
            explain,
            (explain, stage) => getPlanStage(explain.executionStats.executionStages, stage));
    }

    // Comparison of field and constant.
    confirmExpectedExprExecution({$eq: ["$x", 1]}, {nReturned: 1, expectedIndex: {x: 1, y: 1}});
    confirmExpectedExprExecution({$eq: [1, "$x"]}, {nReturned: 1, expectedIndex: {x: 1, y: 1}});

    // $and with both children eligible for index use.
    confirmExpectedExprExecution({$and: [{$eq: ["$x", 2]}, {$eq: ["$y", 2]}]},
                                 {nReturned: 1, expectedIndex: {x: 1, y: 1}});

    // $and with one child eligible for index use and one that is not.
    confirmExpectedExprExecution({$and: [{$eq: ["$x", 1]}, {$eq: ["$x", "$y"]}]},
                                 {nReturned: 1, expectedIndex: {x: 1, y: 1}});

    // $and with one child eligible for index use and a second child containing a $or where one of
    // the two children are eligible.
    confirmExpectedExprExecution(
        {$and: [{$eq: ["$x", 1]}, {$or: [{$eq: ["$x", "$y"]}, {$eq: ["$y", 1]}]}]},
        {nReturned: 1, expectedIndex: {x: 1, y: 1}});

    // Equality comparison against non-multikey dotted path field is expected to use an index.
    confirmExpectedExprExecution({$eq: ["$c.d", 1]}, {nReturned: 1, expectedIndex: {"c.d": 1}});

    // $lt, $lte, $gt, $gte, $in, $ne, and $cmp are not expected to use an index. This is because we
    // have not yet implemented a rewrite of these operators to indexable MatchExpression.
    confirmExpectedExprExecution({$lt: ["$x", 1]}, {nReturned: 20});
    confirmExpectedExprExecution({$lt: [1, "$x"]}, {nReturned: 2});
    confirmExpectedExprExecution({$lte: ["$x", 1]}, {nReturned: 21});
    confirmExpectedExprExecution({$lte: [1, "$x"]}, {nReturned: 3});
    confirmExpectedExprExecution({$gt: ["$x", 1]}, {nReturned: 2});
    confirmExpectedExprExecution({$gt: [1, "$x"]}, {nReturned: 20});
    confirmExpectedExprExecution({$gte: ["$x", 1]}, {nReturned: 3});
    confirmExpectedExprExecution({$gte: [1, "$x"]}, {nReturned: 21});
    confirmExpectedExprExecution({$in: ["$x", [1, 3]]}, {nReturned: 2});
    confirmExpectedExprExecution({$cmp: ["$x", 1]}, {nReturned: 22});
    confirmExpectedExprExecution({$ne: ["$x", 1]}, {nReturned: 22});

    // Comparison with an array value is not expected to use an index.
    confirmExpectedExprExecution({$eq: ["$a.b", [1]]}, {nReturned: 2});
    confirmExpectedExprExecution({$eq: ["$w", [1]]}, {nReturned: 0});

    // A constant expression is not expected to use an index.
    confirmExpectedExprExecution(1, {nReturned: 23});
    confirmExpectedExprExecution(false, {nReturned: 0});
    confirmExpectedExprExecution({$eq: [1, 1]}, {nReturned: 23});
    confirmExpectedExprExecution({$eq: [0, 1]}, {nReturned: 0});

    // Comparison of 2 fields is not expected to use an index.
    confirmExpectedExprExecution({$eq: ["$x", "$y"]}, {nReturned: 20});

    // Comparison against multikey field not expected to use an index.
    confirmExpectedExprExecution({$eq: ["$a.b", 1]}, {nReturned: 1});
    confirmExpectedExprExecution({$eq: ["$e.f", [1]]}, {nReturned: 1});
    confirmExpectedExprExecution({$eq: ["$e.f", 1]}, {nReturned: 0});
    confirmExpectedExprExecution({$eq: ["$g.h", [1]]}, {nReturned: 1});
    confirmExpectedExprExecution({$eq: ["$g.h", 1]}, {nReturned: 0});

    // Comparison against a non-multikey field of a multikey index can use an index, on storage
    // engines other than MMAPv1.
    const metricsToCheck = {nReturned: 1};
    if (!isMMAPv1) {
        metricsToCheck.expectedIndex = {i: 1, j: 1};
    }
    confirmExpectedExprExecution({$eq: ["$i", 1]}, metricsToCheck);
    metricsToCheck.nReturned = 0;
    confirmExpectedExprExecution({$eq: ["$i", 2]}, metricsToCheck);

    // Equality to NaN can use an index.
    confirmExpectedExprExecution({$eq: ["$w", NaN]}, {nReturned: 1, expectedIndex: {w: 1}});

    // Equality to undefined and equality to missing cannot use an index.
    confirmExpectedExprExecution({$eq: ["$w", undefined]}, {nReturned: 16});
    confirmExpectedExprExecution({$eq: ["$w", "$$REMOVE"]}, {nReturned: 16});

    // Equality to null can use an index.
    confirmExpectedExprExecution({$eq: ["$w", null]}, {nReturned: 1, expectedIndex: {w: 1}});

    // Equality inside a nested object can use a non-multikey index.
    confirmExpectedExprExecution({$eq: ["$w.z", 2]}, {nReturned: 1, expectedIndex: {"w.z": 1}});

    // Test that the collation is respected. Since the collations do not match, we should not use
    // the index.
    const caseInsensitiveCollation = {locale: "en_US", strength: 2};
    if (db.getMongo().useReadCommands()) {
        confirmExpectedExprExecution(
            {$eq: ["$w", "FoO"]}, {nReturned: 2}, caseInsensitiveCollation);
    }

    // Test equality queries against a hashed index.
    assert.commandWorked(coll.dropIndex({w: 1}));
    assert.commandWorked(coll.createIndex({w: "hashed"}));
    confirmExpectedExprExecution({$eq: ["$w", 123]}, {nReturned: 1, expectedIndex: {w: "hashed"}});
    confirmExpectedExprExecution({$eq: ["$w", null]}, {nReturned: 1, expectedIndex: {w: "hashed"}});
    confirmExpectedExprExecution({$eq: ["$w", NaN]}, {nReturned: 1, expectedIndex: {w: "hashed"}});
    confirmExpectedExprExecution({$eq: ["$w", undefined]}, {nReturned: 16});
    confirmExpectedExprExecution({$eq: ["$w", "$$REMOVE"]}, {nReturned: 16});

    // Test that equality to null queries can use a sparse index.
    assert.commandWorked(coll.dropIndex({w: "hashed"}));
    assert.commandWorked(coll.createIndex({w: 1}, {sparse: true}));
    confirmExpectedExprExecution({$eq: ["$w", null]}, {nReturned: 1, expectedIndex: {w: 1}});

    // Equality match against text index prefix is expected to fail. Equality predicates are
    // required against the prefix fields of a text index, but currently $eq inside $expr does not
    // qualify.
    assert.throws(() =>
                      coll.aggregate([{$match: {$expr: {$eq: ["$k", 1]}, $text: {$search: "abc"}}}])
                          .itcount());

    // Test that equality match in $expr respects the collection's default collation, both when
    // there is an index with a matching collation and when there isn't.
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assert.commandWorked(
        db.createCollection(coll.getName(), {collation: caseInsensitiveCollation}));
    assert.writeOK(coll.insert({a: "foo", b: "bar"}));
    assert.writeOK(coll.insert({a: "FOO", b: "BAR"}));
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}, {collation: {locale: "simple"}}));

    confirmExpectedExprExecution({$eq: ["$a", "foo"]}, {nReturned: 2, expectedIndex: {a: 1}});
    confirmExpectedExprExecution({$eq: ["$b", "bar"]}, {nReturned: 2});
})();
