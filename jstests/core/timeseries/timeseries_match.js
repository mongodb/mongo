/**
 * Tests $match usage of block processing for time series.
 * @tags: [
 *   requires_timeseries,
 *   does_not_support_stepdowns,
 *   directly_against_shardsvrs_incompatible,
 *   # During fcv upgrade/downgrade the engine might not be what we expect.
 *   cannot_run_during_upgrade_downgrade,
 *   # "Explain of a resolved view must be executed by mongos"
 *   directly_against_shardsvrs_incompatible,
 *   # Some suites use mixed-binary cluster setup where some nodes might have the flag enabled while
 *   # others -- not. For this test we need control over whether the flag is set on the node that
 *   # ends up executing the query.
 *   assumes_standalone_mongod
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getEngine, getQueryPlanner, getSingleNodeExplain} from "jstests/libs/analyze_plan.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js"
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

TimeseriesTest.run((insert) => {
    const datePrefix = 1680912440;

    let coll = db.timeseries_match;
    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

    const timeFieldName = 'time';
    const metaFieldName = 'measurement';

    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName, metaField: metaFieldName},
    }));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    insert(coll, {
        _id: 0,
        [timeFieldName]: new Date(datePrefix + 100),
        [metaFieldName]: "cpu",
        topLevelScalar: 123,
        topLevelArray: [1, 2, 3, 4],
        arrOfObj: [{x: 1}, {x: 2}, {x: 3}, {x: 4}],
    });
    insert(coll, {
        _id: 1,
        [timeFieldName]: new Date(datePrefix + 200),
        [metaFieldName]: "cpu",
        topLevelScalar: 456,
        topLevelArray: [101, 102, 103, 104],
        arrOfObj: [{x: 101}, {x: 102}, {x: 103}, {x: 104}],
    });
    insert(coll, {
        _id: 2,
        [timeFieldName]: new Date(datePrefix + 300),
        [metaFieldName]: "cpu",

        // All fields missing.
    });
    insert(coll, {
        _id: 3,
        [timeFieldName]: new Date(datePrefix + 400),
        [metaFieldName]: "cpu",
        // All fields missing, except topLevelArray, which contains arrays of arrays.
        topLevelArray: [[101, 102], [103, 104], [[105]]],
    });
    insert(coll, {
        _id: 4,
        [timeFieldName]: new Date(datePrefix + 500),
        [metaFieldName]: "cpu",
        // Different schema from above.
        arrOfObj: [{x: [101, 102, 103]}, {x: [104]}],
    });

    const kTestCases = [
        {pred: {"topLevelScalar": {$gt: 123}}, ids: [1], usesBlockProcessing: true},
        {pred: {"topLevelScalar": {$gte: 123}}, ids: [0, 1], usesBlockProcessing: true},
        {pred: {"topLevelScalar": {$lt: 456}}, ids: [0], usesBlockProcessing: true},
        {pred: {"topLevelScalar": {$lte: 456}}, ids: [0, 1], usesBlockProcessing: true},
        {pred: {"topLevelScalar": {$eq: 456}}, ids: [1], usesBlockProcessing: true},
        {pred: {"topLevelScalar": {$ne: 456}}, ids: [0, 2, 3, 4], usesBlockProcessing: true},

        {pred: {"topLevelArray": {$gt: 4}}, ids: [1], usesBlockProcessing: true},
        {pred: {"topLevelArray": {$gte: 4}}, ids: [0, 1], usesBlockProcessing: true},
        {pred: {"topLevelArray": {$lt: 101}}, ids: [0], usesBlockProcessing: true},
        {pred: {"topLevelArray": {$lte: 101}}, ids: [0, 1], usesBlockProcessing: true},
        {pred: {"topLevelArray": {$eq: 102}}, ids: [1], usesBlockProcessing: true},

        {pred: {"arrOfObj.x": {$gt: 4}}, ids: [1, 4], usesBlockProcessing: true},
        {pred: {"arrOfObj.x": {$gte: 4}}, ids: [0, 1, 4], usesBlockProcessing: true},
        {pred: {"arrOfObj.x": {$lt: 101}}, ids: [0], usesBlockProcessing: true},
        {pred: {"arrOfObj.x": {$lte: 101}}, ids: [0, 1, 4], usesBlockProcessing: true},
        {pred: {"arrOfObj.x": {$eq: 102}}, ids: [1, 4], usesBlockProcessing: true},
        {pred: {"arrOfObj.x": {$ne: 102}}, ids: [0, 2, 3], usesBlockProcessing: true},

        {
            pred: {"time": {$gt: new Date(datePrefix + 100)}},
            ids: [1, 2, 3, 4],
            usesBlockProcessing: true
        },
        {
            pred: {"time": {$gte: new Date(datePrefix + 100)}},
            ids: [0, 1, 2, 3, 4],
            usesBlockProcessing: true
        },
        {pred: {"time": {$lt: new Date(datePrefix + 200)}}, ids: [0], usesBlockProcessing: true},
        {
            pred: {"time": {$lte: new Date(datePrefix + 200)}},
            ids: [0, 1],
            usesBlockProcessing: true
        },
        {pred: {"time": {$eq: new Date(datePrefix + 300)}}, ids: [2], usesBlockProcessing: true},
        {
            pred: {"time": {$ne: new Date(datePrefix + 200)}},
            ids: [0, 2, 3, 4],
            usesBlockProcessing: true
        },
        {
            pred: {"time": {$gt: new Date(datePrefix + 100), $lt: new Date(datePrefix + 300)}},
            ids: [1],
            usesBlockProcessing: true
        },
        {
            pred: {"time": {$eq: {"obj": new Date(datePrefix + 100)}}},
            ids: [],
            usesBlockProcessing: true
        },

        // Equality to array does not use block processing.
        {pred: {"topLevelArray": {$eq: [101, 102]}}, ids: [3], usesBlockProcessing: false},
        {pred: {"topLevelScalar": {$eq: [999, 999]}}, ids: [], usesBlockProcessing: false}
    ];

    // $match pushdown requires sbe to be fully enabled and featureFlagTimeSeriesInSbe to be set.
    const sbeEnabled = checkSbeFullyEnabled(db) &&
        FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'TimeSeriesInSbe');
    for (let testCase of kTestCases) {
        const pipe = [{$match: testCase.pred}, {$project: {_id: 1}}];

        // Check results.
        {
            const results = coll.aggregate(pipe).toArray().map((x) => x._id)
            results.sort();
            assert.eq(testCase.ids, results, () => "Test case " + tojson(testCase));
        }

        // Check that explain indicates block processing is being used. This is a best effort
        // check.
        const explain = coll.explain().aggregate(pipe);
        const engineUsed = getEngine(explain);
        const singleNodeQueryPlanner = getQueryPlanner(getSingleNodeExplain(explain));
        printjson(singleNodeQueryPlanner);
        function testCaseAndExplainFn(description) {
            return () => description + " for test case " + tojson(testCase) +
                " failed with explain " + tojson(singleNodeQueryPlanner);
        }

        if (sbeEnabled) {
            const sbePlan = singleNodeQueryPlanner.winningPlan.slotBasedPlan.stages;

            if (testCase.usesBlockProcessing) {
                assert.eq(engineUsed, "sbe");

                // Check for the fold function.
                assert(sbePlan.includes("cellFoldValues_F"),
                       testCaseAndExplainFn("Expected explain to use block processing"));
            } else {
                assert(!sbePlan.includes("cellFoldValues_F"),
                       testCaseAndExplainFn("Expected explain not to use block processing"));
            }
        }
    }

    // Special test case which can result in an empty event filter being compiled (SERVER-84001).
    {
        const pipe = [
            {$match: {"topLevelScalarField": {$not: {$in: []}}}},
            {$match: {"measurement": "cpu"}},
            {$project: {_id: 1}}
        ];
        const res = coll.aggregate(pipe).toArray()
        assert.eq(res.length, coll.count());
    }
});
