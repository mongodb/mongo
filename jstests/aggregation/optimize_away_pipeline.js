// Tests that an aggregation pipeline can be optimized away and the query can be answered using
// just the query layer if the pipeline has only one $cursor source, or if the pipeline can be
// collapsed into a single $cursor source pipeline. The resulting cursor in this case will look
// like what the client would have gotten from find command.
//
// Relies on the pipeline stages to be collapsed into a single $cursor stage, so pipelines cannot
// be wrapped into a facet stage to not prevent this optimization.
// TODO SERVER-40323: Plan analyzer helper functions cannot correctly handle explain output for
// sharded collections.
// @tags: [do_not_wrap_aggregations_in_facets, assumes_unsharded_collection]
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For 'orderedArrayEq' and 'arrayEq'.
    load("jstests/concurrency/fsm_workload_helpers/server_types.js");  // For isWiredTiger.
    load("jstests/libs/analyze_plan.js");     // For 'aggPlanHasStage' and other explain helpers.
    load("jstests/libs/fixture_helpers.js");  // For 'isMongos' and 'isSharded'.

    const coll = db.optimize_away_pipeline;
    coll.drop();
    assert.writeOK(coll.insert({_id: 1, x: 10}));
    assert.writeOK(coll.insert({_id: 2, x: 20}));
    assert.writeOK(coll.insert({_id: 3, x: 30}));

    // Asserts that the give pipeline has *not* been optimized away and the request is answered
    // using the aggregation module. There should be pipeline stages present in the explain output.
    // The functions also asserts that a query stage passed in the 'stage' argument is present in
    // the explain output. If 'expectedResult' is provided, the pipeline is executed and the
    // returned result as validated agains the expected result without respecting the order of the
    // documents. If 'preserveResultOrder' is 'true' - the order is respected.
    function assertPipelineUsesAggregation({
        pipeline = [],
        pipelineOptions = {},
        expectedStage = null,
        expectedResult = null,
        preserveResultOrder = false
    } = {}) {
        const explainOutput = coll.explain().aggregate(pipeline, pipelineOptions);

        assert(isAggregationPlan(explainOutput),
               "Expected pipeline " + tojsononeline(pipeline) +
                   " to use an aggregation framework in the explain output: " +
                   tojson(explainOutput));
        assert(!isQueryPlan(explainOutput),
               "Expected pipeline " + tojsononeline(pipeline) +
                   " *not* to use a query layer at the root level in the explain output: " +
                   tojson(explainOutput));

        let cursor = getAggPlanStage(explainOutput, "$cursor");
        if (cursor) {
            cursor = cursor.$cursor;
        } else {
            cursor = getAggPlanStage(explainOutput, "$geoNearCursor").$geoNearCursor;
        }

        assert(cursor,
               "Expected pipeline " + tojsononeline(pipeline) + " to include a $cursor " +
                   " stage in the explain output: " + tojson(explainOutput));
        assert(cursor.queryPlanner.optimizedPipeline === undefined,
               "Expected pipeline " + tojsononeline(pipeline) + " to *not* include an " +
                   "'optimizedPipeline' field in the explain output: " + tojson(explainOutput));
        assert(aggPlanHasStage(explainOutput, expectedStage),
               "Expected pipeline " + tojsononeline(pipeline) + " to include a " + expectedStage +
                   " stage in the explain output: " + tojson(explainOutput));

        if (expectedResult) {
            const actualResult = coll.aggregate(pipeline, pipelineOptions).toArray();
            assert(preserveResultOrder ? orderedArrayEq(actualResult, expectedResult)
                                       : arrayEq(actualResult, expectedResult));
        }

        return explainOutput;
    }

    // Asserts that the give pipeline has been optimized away and the request is answered using
    // just the query module. There should be no pipeline stages present in the explain output.
    // The functions also asserts that a query stage passed in the 'stage' argument is present in
    // the explain output. If 'expectedResult' is provided, the pipeline is executed and the
    // returned result as validated agains the expected result without respecting the order of the
    // documents. If 'preserveResultOrder' is 'true' - the order is respected.
    function assertPipelineDoesNotUseAggregation({
        pipeline = [],
        pipelineOptions = {},
        expectedStage = null,
        expectedResult = null,
        preserveResultOrder = false
    } = {}) {
        const explainOutput = coll.explain().aggregate(pipeline, pipelineOptions);

        assert(!isAggregationPlan(explainOutput),
               "Expected pipeline " + tojsononeline(pipeline) +
                   " *not* to use an aggregation framework in the explain output: " +
                   tojson(explainOutput));
        assert(isQueryPlan(explainOutput),
               "Expected pipeline " + tojsononeline(pipeline) +
                   " to use a query layer at the root level in the explain output: " +
                   tojson(explainOutput));
        if (explainOutput.hasOwnProperty("shards")) {
            Object.keys(explainOutput.shards)
                .forEach((shard) => assert(
                             explainOutput.shards[shard].queryPlanner.optimizedPipeline === true,
                             "Expected pipeline " + tojsononeline(pipeline) + " to include an " +
                                 "'optimizedPipeline' field in the explain output: " +
                                 tojson(explainOutput)));
        } else {
            assert(explainOutput.queryPlanner.optimizedPipeline === true,
                   "Expected pipeline " + tojsononeline(pipeline) + " to include an " +
                       "'optimizedPipeline' field in the explain output: " + tojson(explainOutput));
        }
        assert(planHasStage(db, explainOutput, expectedStage),
               "Expected pipeline " + tojsononeline(pipeline) + " to include a " + expectedStage +
                   " stage in the explain output: " + tojson(explainOutput));

        if (expectedResult) {
            const actualResult = coll.aggregate(pipeline, pipelineOptions).toArray();
            assert(preserveResultOrder ? orderedArrayEq(actualResult, expectedResult)
                                       : arrayEq(actualResult, expectedResult));
        }

        return explainOutput;
    }

    // Test that getMore works with the optimized query.
    function testGetMore({command = null, expectedResult = null} = {}) {
        const documents =
            new DBCommandCursor(db, assert.commandWorked(db.runCommand(command)), 1 /* batchsize */)
                .toArray();
        assert(arrayEq(documents, expectedResult));
    }

    let explainOutput;

    // Basic pipelines.

    // Test basic scenarios when a pipeline has a single $cursor stage or can be collapsed into a
    // single cursor stage.
    assertPipelineDoesNotUseAggregation({
        pipeline: [],
        expectedStage: "COLLSCAN",
        expectedResult: [{_id: 1, x: 10}, {_id: 2, x: 20}, {_id: 3, x: 30}]
    });
    assertPipelineDoesNotUseAggregation({
        pipeline: [{$match: {x: 20}}],
        expectedStage: "COLLSCAN",
        expectedResult: [{_id: 2, x: 20}]
    });

    // Pipelines with a collation.

    // Test a simple pipeline with a case-insensitive collation.
    assert.writeOK(coll.insert({_id: 4, x: 40, b: "abc"}));
    assertPipelineDoesNotUseAggregation({
        pipeline: [{$match: {b: "ABC"}}],
        pipelineOptions: {collation: {locale: "en_US", strength: 2}},
        expectedStage: "COLLSCAN",
        expectedResult: [{_id: 4, x: 40, b: "abc"}]
    });
    assert.commandWorked(coll.deleteOne({_id: 4}));

    // Pipelines with covered queries.

    // We can collapse a covered query into a single $cursor when $project and $sort are present and
    // the latter is near the front of the pipeline. Skip this test in sharded modes as we cannot
    // correctly handle explain output in plan analyzer helper functions.
    assert.commandWorked(coll.createIndex({x: 1}));
    assertPipelineDoesNotUseAggregation({
        pipeline: [{$sort: {x: 1}}, {$project: {x: 1, _id: 0}}],
        expectedStage: "IXSCAN",
        expectedResult: [{x: 10}, {x: 20}, {x: 30}],
        preserveResultOrder: true
    });
    assertPipelineDoesNotUseAggregation({
        pipeline: [{$match: {x: {$gte: 20}}}, {$sort: {x: 1}}, {$project: {x: 1, _id: 0}}],
        expectedStage: "IXSCAN",
        expectedResult: [{x: 20}, {x: 30}],
        preserveResultOrder: true
    });
    // TODO: SERVER-36723 We cannot collapse if there is a $limit stage though.
    assertPipelineUsesAggregation({
        pipeline:
            [{$match: {x: {$gte: 20}}}, {$sort: {x: 1}}, {$limit: 1}, {$project: {x: 1, _id: 0}}],
        expectedStage: "IXSCAN",
        expectedResult: [{x: 20}]
    });
    assert.commandWorked(coll.dropIndexes());

    // Pipelines which cannot be optimized away.

    // TODO SERVER-40254: Uncovered queries.
    assert.writeOK(coll.insert({_id: 4, x: 40, a: {b: "ab1"}}));
    assertPipelineUsesAggregation({
        pipeline: [{$project: {x: 1, _id: 0}}],
        expectedStage: "COLLSCAN",
        expectedResult: [{x: 10}, {x: 20}, {x: 30}, {x: 40}]
    });
    assertPipelineUsesAggregation({
        pipeline: [{$match: {x: 20}}, {$project: {x: 1, _id: 0}}],
        expectedStage: "COLLSCAN",
        expectedResult: [{x: 20}]
    });
    assertPipelineUsesAggregation({
        pipeline: [{$project: {x: 1, "a.b": 1, _id: 0}}],
        expectedStage: "COLLSCAN",
        expectedResult: [{x: 10}, {x: 20}, {x: 30}, {x: 40, a: {b: "ab1"}}]
    });
    assertPipelineUsesAggregation({
        pipeline: [{$match: {x: 40}}, {$project: {"a.b": 1, _id: 0}}],
        expectedStage: "COLLSCAN",
        expectedResult: [{a: {b: "ab1"}}]
    });
    assert.commandWorked(coll.deleteOne({_id: 4}));

    // TODO SERVER-36723: $limit stage is not supported yet.
    assertPipelineUsesAggregation({
        pipeline: [{$match: {x: 20}}, {$limit: 1}],
        expectedStage: "COLLSCAN",
        expectedResult: [{_id: 2, x: 20}]
    });
    // TODO SERVER-36723: $skip stage is not supported yet.
    assertPipelineUsesAggregation({
        pipeline: [{$match: {x: {$gte: 20}}}, {$skip: 1}],
        expectedStage: "COLLSCAN",
        expectedResult: [{_id: 3, x: 30}]
    });
    // We cannot collapse a $project stage if it has a complex pipeline expression.
    assertPipelineUsesAggregation(
        {pipeline: [{$project: {x: {$substr: ["$y", 0, 1]}, _id: 0}}], expectedStage: "COLLSCAN"});
    assertPipelineUsesAggregation({
        pipeline: [{$match: {x: 20}}, {$project: {x: {$substr: ["$y", 0, 1]}, _id: 0}}],
        expectedStage: "COLLSCAN"
    });
    // We cannot optimize away a pipeline if there are stages which have no equivalent in the
    // find command.
    assertPipelineUsesAggregation({
        pipeline: [{$match: {x: {$gte: 20}}}, {$count: "count"}],
        expectedStage: "COLLSCAN",
        expectedResult: [{count: 2}]
    });
    assertPipelineUsesAggregation({
        pipeline: [{$match: {x: {$gte: 20}}}, {$group: {_id: "null", s: {$sum: "$x"}}}],
        expectedStage: "COLLSCAN",
        expectedResult: [{_id: "null", s: 50}]
    });
    // TODO SERVER-40253: We cannot optimize away text search queries.
    assert.commandWorked(coll.createIndex({y: "text"}));
    assertPipelineUsesAggregation(
        {pipeline: [{$match: {$text: {$search: "abc"}}}], expectedStage: "IXSCAN"});
    assert.commandWorked(coll.dropIndexes());
    // We cannot optimize away geo near queries.
    assert.commandWorked(coll.createIndex({"y": "2d"}));
    assertPipelineUsesAggregation({
        pipeline: [{$geoNear: {near: [0, 0], distanceField: "y", spherical: true}}],
        expectedStage: "GEO_NEAR_2D"
    });
    assert.commandWorked(coll.dropIndexes());

    // getMore cases.

    // Test getMore on a collection with an optimized away pipeline.
    testGetMore({
        command: {aggregate: coll.getName(), pipeline: [], cursor: {batchSize: 1}},
        expectedResult: [{_id: 1, x: 10}, {_id: 2, x: 20}, {_id: 3, x: 30}]
    });
    testGetMore({
        command: {
            aggregate: coll.getName(),
            pipeline: [{$match: {x: {$gte: 20}}}],
            cursor: {batchSize: 1}
        },
        expectedResult: [{_id: 2, x: 20}, {_id: 3, x: 30}]
    });
    testGetMore({
        command: {
            aggregate: coll.getName(),
            pipeline: [{$match: {x: {$gte: 20}}}, {$project: {x: 1, _id: 0}}],
            cursor: {batchSize: 1}
        },
        expectedResult: [{x: 20}, {x: 30}]
    });
    // Test getMore on a view with an optimized away pipeline. Since views cannot be created when
    // imlicit sharded collection mode is on, this test will be run only on a non-sharded
    // collection.
    let view;
    if (!FixtureHelpers.isSharded(coll)) {
        view = db.optimize_away_pipeline_view;
        view.drop();
        assert.commandWorked(db.createView(view.getName(), coll.getName(), []));
        testGetMore({
            command: {find: view.getName(), filter: {}, batchSize: 1},
            expectedResult: [{_id: 1, x: 10}, {_id: 2, x: 20}, {_id: 3, x: 30}]
        });
    }
    // Test getMore puts a correct namespace into profile data for a colletion with optimized away
    // pipeline. Cannot be run on mongos as profiling can be enabled only on mongod. Also profiling
    // is supported on WiredTiger only.
    if (!FixtureHelpers.isMongos(db) && isWiredTiger(db)) {
        db.system.profile.drop();
        db.setProfilingLevel(2);
        testGetMore({
            command: {
                aggregate: coll.getName(),
                pipeline: [{$match: {x: 10}}],
                cursor: {batchSize: 1},
                comment: 'optimize_away_pipeline'
            },
            expectedResult: [{_id: 1, x: 10}]
        });
        db.setProfilingLevel(0);
        let profile = db.system.profile.find({}, {op: 1, ns: 1, comment: 'optimize_away_pipeline'})
                          .sort({ts: 1})
                          .toArray();
        assert(arrayEq(
            profile,
            [{op: "command", ns: coll.getFullName()}, {op: "getmore", ns: coll.getFullName()}]));
        // Test getMore puts a correct namespace into profile data for a view with an optimized away
        // pipeline.
        if (!FixtureHelpers.isSharded(coll)) {
            db.system.profile.drop();
            db.setProfilingLevel(2);
            testGetMore({
                command: {
                    find: view.getName(),
                    filter: {x: 10},
                    batchSize: 1,
                    comment: 'optimize_away_pipeline'
                },
                expectedResult: [{_id: 1, x: 10}]
            });
            db.setProfilingLevel(0);
            profile = db.system.profile.find({}, {op: 1, ns: 1, comment: 'optimize_away_pipeline'})
                          .sort({ts: 1})
                          .toArray();
            assert(arrayEq(
                profile,
                [{op: "query", ns: view.getFullName()}, {op: "getmore", ns: view.getFullName()}]));
        }
    }
}());
