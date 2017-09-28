// Confirms expected index use when performing a match with a $expr statement.

(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");

    const coll = db.expr_index_use;
    coll.drop();

    assert.writeOK(coll.insert({x: 0}));
    assert.writeOK(coll.insert({x: 1, y: 1}));
    assert.writeOK(coll.insert({x: 2, y: 2}));
    assert.writeOK(coll.insert({x: 3, y: 10}));
    assert.writeOK(coll.insert({y: 20}));
    assert.writeOK(coll.insert({a: {b: 1}}));
    assert.writeOK(coll.insert({a: {b: [1]}}));
    assert.writeOK(coll.insert({a: [{b: 1}]}));
    assert.writeOK(coll.insert({a: [{b: [1]}]}));

    assert.commandWorked(coll.createIndex({x: 1, y: 1}));
    assert.commandWorked(coll.createIndex({y: 1}));
    assert.commandWorked(coll.createIndex({"a.b": 1}));

    /**
     * Executes an 'executionStats' explain on pipeline and confirms 'metricsToCheck' which is an
     * object containing:
     *  - nReturned:        The number of documents the pipeline is expected to return.
     *  - expectedIndex:    Either an index specification object when index use is expected or
     *                      'null' if a collection scan is expected.
     *  - indexBounds:      The expected index bounds.
     */
    function confirmExpectedPipelineExecution(pipeline, metricsToCheck) {
        assert(metricsToCheck.hasOwnProperty("nReturned"),
               "metricsToCheck must contain an nReturned field");

        assert.eq(metricsToCheck.nReturned, coll.aggregate(pipeline).itcount());

        const explain = assert.commandWorked(coll.explain("executionStats").aggregate(pipeline));
        if (metricsToCheck.hasOwnProperty("expectedIndex")) {
            const stage = getAggPlanStage(explain, "IXSCAN");
            assert.neq(null, stage, tojson(explain));
            assert(stage.hasOwnProperty("keyPattern"), tojson(explain));
            assert.docEq(stage.keyPattern, metricsToCheck.expectedIndex, tojson(explain));
        } else {
            assert.neq(null, aggPlanHasStage(explain, "COLLSCAN"), tojson(explain));
        }

        if (metricsToCheck.hasOwnProperty("indexBounds")) {
            const stage = getAggPlanStage(explain, "IXSCAN");
            assert.neq(null, stage, tojson(explain));
            assert(stage.hasOwnProperty("indexBounds"), tojson(explain));
            assert.docEq(stage.indexBounds, metricsToCheck.indexBounds, tojson(explain));
        }
    }

    // Comparison of field and constant.
    confirmExpectedPipelineExecution([{$match: {$expr: {$eq: ["$x", 1]}}}], {
        nReturned: 1,
        expectedIndex: {x: 1, y: 1},
        indexBounds: {"x": ["[1.0, 1.0]"], "y": ["[MinKey, MaxKey]"]}
    });
    confirmExpectedPipelineExecution([{$match: {$expr: {$eq: [1, "$x"]}}}], {
        nReturned: 1,
        expectedIndex: {x: 1, y: 1},
        indexBounds: {"x": ["[1.0, 1.0]"], "y": ["[MinKey, MaxKey]"]}
    });
    confirmExpectedPipelineExecution([{$match: {$expr: {$lt: ["$x", 1]}}}], {
        nReturned: 1,
        expectedIndex: {x: 1, y: 1},
        indexBounds: {"x": ["[-inf.0, 1.0)"], "y": ["[MinKey, MaxKey]"]}
    });
    confirmExpectedPipelineExecution([{$match: {$expr: {$lt: [1, "$x"]}}}], {
        nReturned: 2,
        expectedIndex: {x: 1, y: 1},
        indexBounds: {"x": ["(1.0, inf.0]"], "y": ["[MinKey, MaxKey]"]}
    });
    confirmExpectedPipelineExecution([{$match: {$expr: {$lte: ["$x", 1]}}}], {
        nReturned: 2,
        expectedIndex: {x: 1, y: 1},
        indexBounds: {"x": ["[-inf.0, 1.0]"], "y": ["[MinKey, MaxKey]"]}
    });
    confirmExpectedPipelineExecution([{$match: {$expr: {$lte: [1, "$x"]}}}], {
        nReturned: 3,
        expectedIndex: {x: 1, y: 1},
        indexBounds: {"x": ["[1.0, inf.0]"], "y": ["[MinKey, MaxKey]"]}
    });
    confirmExpectedPipelineExecution([{$match: {$expr: {$gt: ["$x", 1]}}}], {
        nReturned: 2,
        expectedIndex: {x: 1, y: 1},
        indexBounds: {"x": ["(1.0, inf.0]"], "y": ["[MinKey, MaxKey]"]}
    });
    confirmExpectedPipelineExecution([{$match: {$expr: {$gt: [1, "$x"]}}}], {
        nReturned: 1,
        expectedIndex: {x: 1, y: 1},
        indexBounds: {"x": ["[-inf.0, 1.0)"], "y": ["[MinKey, MaxKey]"]}
    });
    confirmExpectedPipelineExecution([{$match: {$expr: {$gte: ["$x", 1]}}}], {
        nReturned: 3,
        expectedIndex: {x: 1, y: 1},
        indexBounds: {"x": ["[1.0, inf.0]"], "y": ["[MinKey, MaxKey]"]}
    });
    confirmExpectedPipelineExecution([{$match: {$expr: {$gte: [1, "$x"]}}}], {
        nReturned: 2,
        expectedIndex: {x: 1, y: 1},
        indexBounds: {"x": ["[-inf.0, 1.0]"], "y": ["[MinKey, MaxKey]"]}
    });

    // $in with field and array of values.
    confirmExpectedPipelineExecution([{$match: {$expr: {$in: ["$x", [1, 3]]}}}], {
        nReturned: 2,
        expectedIndex: {x: 1, y: 1},
        indexBounds: {"x": ["[1.0, 1.0]", "[3.0, 3.0]"], "y": ["[MinKey, MaxKey]"]}
    });

    // $and with both children eligible for index use.
    confirmExpectedPipelineExecution(
        [{$match: {$expr: {$and: [{$eq: ["$x", 2]}, {$gt: ["$y", 1]}]}}}], {
            nReturned: 1,
            expectedIndex: {x: 1, y: 1},
            indexBounds: {"x": ["[2.0, 2.0]"], "y": ["(1.0, inf.0]"]}
        });

    // $and with one child eligible for index use and one that is not.
    confirmExpectedPipelineExecution(
        [{$match: {$expr: {$and: [{$gt: ["$x", 1]}, {$eq: ["$x", "$y"]}]}}}], {
            nReturned: 1,
            expectedIndex: {x: 1, y: 1},
            indexBounds: {"x": ["(1.0, inf.0]"], "y": ["[MinKey, MaxKey]"]}
        });

    // $and with one child elibible for index use and a second child containing a $or where one of
    // the two children are eligible.
    confirmExpectedPipelineExecution(
        [
          {
            $match: {
                $expr:
                    {$and: [{$gt: ["$x", 1]}, {$or: [{$eq: ["$x", "$y"]}, {$gt: ["$y", 1]}]}]}
            }
          }
        ],
        {
          nReturned: 2,
          expectedIndex: {x: 1, y: 1},
          indexBounds: {"x": ["(1.0, inf.0]"], "y": ["[MinKey, MaxKey]"]}
        });

    // $cmp is not expected to use an index.
    confirmExpectedPipelineExecution([{$match: {$expr: {$cmp: ["$x", 1]}}}], {nReturned: 8});

    // An constant expression is not expected to use an index.
    confirmExpectedPipelineExecution([{$match: {$expr: 1}}], {nReturned: 9});

    // Comparison of 2 fields is not expected to use an index.
    confirmExpectedPipelineExecution([{$match: {$expr: {$eq: ["$x", "$y"]}}}], {nReturned: 6});

    // Comparison with field path length > 1 is not expected to use an index.
    confirmExpectedPipelineExecution([{$match: {$expr: {$eq: ["$a.b", 1]}}}], {nReturned: 1});

    // $in with field path length > 1 is not expected to use an index.
    confirmExpectedPipelineExecution([{$match: {$expr: {$in: ["$a.b", [1, 3]]}}}], {nReturned: 1});
})();
