/**
 * Tests $match usage of block processing for time series in multithreaded scenario
 * @tags: [
 *   requires_timeseries,
 *   does_not_support_stepdowns,
 *   directly_against_shardsvrs_incompatible,
 *   cannot_run_during_upgrade_downgrade,
 *   assumes_standalone_mongod
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getEngine, getQueryPlanner, getSingleNodeExplain} from "jstests/libs/analyze_plan.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

function runAndAssertAggregation(coll, pred, ids) {
    const pipe = [{$match: pred}, {$project: {_id: 1}}];

    const results = coll.aggregate(pipe).toArray().map((x) => x._id);
    results.sort();
    assert.eq(ids, results, () => "Aggregate " + tojson(pipe));
}

function runTests(conn, db, coll, testCases) {
    // $match pushdown requires sbe to be fully enabled and featureFlagTimeSeriesInSbe to be set.
    const sbeEnabled = checkSbeFullyEnabled(db) &&
        FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'TimeSeriesInSbe');

    for (let testCase of testCases) {
        let preds = testCase.preds;
        let ids = testCase.ids;
        let usesBlockProcessing = testCase.usesBlockProcessing;

        // Run and assert the match aggregation with the first predicate
        runAndAssertAggregation(coll, preds[0], ids[0]);

        // Check that explain indicates block processing is being used. This is a best effort check.
        const explain = coll.explain().aggregate([{$match: preds[0]}, {$project: {_id: 1}}]);
        const engineUsed = getEngine(explain);
        const singleNodeQueryPlanner = getQueryPlanner(explain);
        function testCaseAndExplainFn(description) {
            return () => description + " for predicate " + tojson(preds[0]) +
                " failed with explain " + tojson(singleNodeQueryPlanner);
        }

        if (sbeEnabled || singleNodeQueryPlanner.winningPlan.slotBasedPlan) {
            const sbePlan = singleNodeQueryPlanner.winningPlan.slotBasedPlan.stages;

            if (usesBlockProcessing) {
                assert.eq(engineUsed, "sbe");

                // Check for the fold function.
                assert(sbePlan.includes("cellFoldValues_F"),
                       testCaseAndExplainFn("Expected explain to use block processing"));
            } else {
                assert(!sbePlan.includes("cellFoldValues_F"),
                       testCaseAndExplainFn("Expected explain not to use block processing"));
            }
        }

        // Run and assert the aggregations with different constants parallelly in separate threads
        let threads = [];
        for (let i = 0; i < preds.length; i++) {
            let thread = new Thread(function(connStr, i, runAndAssertAggregation, pred, ids) {
                let conn = new Mongo(connStr);
                let db = conn.getDB('test');
                let coll = db.timeseries_multithread;

                // Run each aggregation 500 times to ensure concurrent execution
                for (let times = 0; times < 500; ++times) {
                    runAndAssertAggregation(
                        coll,
                        pred,
                        ids,
                    );
                }
            }, conn.name, i, runAndAssertAggregation, preds[i], ids[i]);
            threads.push(thread);
            thread.start();
        }

        for (let i = 0; i < threads.length; i++) {
            threads[i].join();
        }
    }
}

TimeseriesTest.run((insert) => {
    const datePrefix = 1680912440;

    const conn = MongoRunner.runMongod();
    assert.neq(null, conn);
    const kDbName = "test";
    const db = conn.getDB(kDbName);

    let coll = db.timeseries_multithread;
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
        emptyArray: [],
        nestedArray: [{subField: [1]}, {subField: [2]}, {subField: 3}],
        sometimesDoublyNestedArray: [[1], [2], [3], []]
    });
    insert(coll, {
        _id: 1,
        [timeFieldName]: new Date(datePrefix + 200),
        [metaFieldName]: "cpu",
        topLevelScalar: 456,
        topLevelArray: [101, 102, 103, 104],
        arrOfObj: [{x: 101}, {x: 102}, {x: 103}, {x: 104}],
        emptyArray: [],
        nestedArray: [{subField: [101]}, {subField: [102]}, {subField: 103}],
        sometimesDoublyNestedArray: [[101], 102, [103]]
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
        nestedArray: [{subField: []}, {subField: []}],
        sometimesDoublyNestedArray: [[]],
    });

    const constants = [4, 101, 102, 123, 150, 300, 456, 550];

    const kTestCases = [
        {
            preds: constants.map(constant => ({"topLevelScalar": {$gt: constant}})),
            ids: [[0, 1], [0, 1], [0, 1], [1], [1], [1], [], []],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"topLevelScalar": {$gte: constant}})),
            ids: [[0, 1], [0, 1], [0, 1], [0, 1], [1], [1], [1], []],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"topLevelScalar": {$lt: constant}})),
            ids: [[], [], [], [], [0], [0], [0], [0, 1]],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"topLevelScalar": {$lte: constant}})),
            ids: [[], [], [], [0], [0], [0], [0, 1], [0, 1]],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"topLevelScalar": {$eq: constant}})),
            ids: [[], [], [], [0], [], [], [1], []],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"topLevelScalar": {$ne: constant}})),
            ids: [
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 2, 3, 4],
                [0, 1, 2, 3, 4]
            ],
            usesBlockProcessing: true
        },

        {
            preds: constants.map(constant => ({"topLevelArray": {$gt: constant}})),
            ids: [[1], [1], [1], [], [], [], [], []],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"topLevelArray": {$gte: constant}})),
            ids: [[0, 1], [1], [1], [], [], [], [], []],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"topLevelArray": {$lt: constant}})),
            ids: [[0], [0], [0, 1], [0, 1], [0, 1], [0, 1], [0, 1], [0, 1]],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"topLevelArray": {$lte: constant}})),
            ids: [[0], [0, 1], [0, 1], [0, 1], [0, 1], [0, 1], [0, 1], [0, 1]],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"topLevelArray": {$eq: constant}})),
            ids: [[0], [1], [1], [], [], [], [], []],
            usesBlockProcessing: true
        },

        {
            preds: constants.map(constant => ({"arrOfObj.x": {$gt: constant}})),
            ids: [[1, 4], [1, 4], [1, 4], [], [], [], [], []],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"arrOfObj.x": {$gte: constant}})),
            ids: [[0, 1, 4], [1, 4], [1, 4], [], [], [], [], []],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"arrOfObj.x": {$lt: constant}})),
            ids: [[0], [0], [0, 1, 4], [0, 1, 4], [0, 1, 4], [0, 1, 4], [0, 1, 4], [0, 1, 4]],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"arrOfObj.x": {$lte: constant}})),
            ids: [[0], [0, 1, 4], [0, 1, 4], [0, 1, 4], [0, 1, 4], [0, 1, 4], [0, 1, 4], [0, 1, 4]],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"arrOfObj.x": {$eq: constant}})),
            ids: [[0], [1, 4], [1, 4], [], [], [], [], []],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"arrOfObj.x": {$ne: constant}})),
            ids: [
                [1, 2, 3, 4],
                [0, 2, 3],
                [0, 2, 3],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4]
            ],
            usesBlockProcessing: true
        },

        {
            preds: constants.map(constant => ({"time": {$gt: new Date(datePrefix + constant)}})),
            ids: [
                [0, 1, 2, 3, 4],
                [1, 2, 3, 4],
                [1, 2, 3, 4],
                [1, 2, 3, 4],
                [1, 2, 3, 4],
                [3, 4],
                [4],
                []
            ],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"time": {$gte: new Date(datePrefix + constant)}})),
            ids: [
                [0, 1, 2, 3, 4],
                [1, 2, 3, 4],
                [1, 2, 3, 4],
                [1, 2, 3, 4],
                [1, 2, 3, 4],
                [2, 3, 4],
                [4],
                []
            ],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"time": {$lt: new Date(datePrefix + constant)}})),
            ids: [[], [0], [0], [0], [0], [0, 1], [0, 1, 2, 3], [0, 1, 2, 3, 4]],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"time": {$lte: new Date(datePrefix + constant)}})),
            ids: [[], [0], [0], [0], [0], [0, 1, 2], [0, 1, 2, 3], [0, 1, 2, 3, 4]],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"time": {$eq: new Date(datePrefix + constant)}})),
            ids: [[], [], [], [], [], [2], [], []],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"time": {$ne: new Date(datePrefix + constant)}})),
            ids: [
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4]
            ],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(
                constant => ({
                    "time": {$gt: new Date(datePrefix + constant), $lt: new Date(datePrefix + 300)}
                })),
            ids: [[0, 1], [1], [1], [1], [1], [], [], []],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(
                constant => ({
                    $or: [
                        {
                            $and: [
                                {"time": {$gte: new Date("2019-09-27T21:14:45.654Z")}},
                                {"time": {$gt: new Date(datePrefix + constant)}}
                            ]
                        },
                        {"time": {$eq: new Date(datePrefix + constant)}}
                    ]
                })),
            ids: [[], [], [], [], [], [2], [], []],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(
                constant => ({
                    $or: [
                        {$expr: {$regexFind: {input: "$measurement", regex: "^2", options: ""}}},
                        {"topLevelScalar": {$lte: constant}}
                    ]
                })),
            ids: [[], [], [], [0], [0], [0], [0, 1], [0, 1]],
            usesBlockProcessing: false
        },
        {
            preds: constants.map(constant => ({$expr: {$lt: [constant, "$topLevelScalar"]}})),
            ids: [[0, 1], [0, 1], [0, 1], [1], [1], [1], [], []],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant =>
                                     ({$expr: {$lt: [new Date(datePrefix + constant), "$time"]}})),
            ids: [
                [0, 1, 2, 3, 4],
                [1, 2, 3, 4],
                [1, 2, 3, 4],
                [1, 2, 3, 4],
                [1, 2, 3, 4],
                [3, 4],
                [4],
                []
            ],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant =>
                                     ({$expr: {$lte: [new Date(datePrefix + constant), "$time"]}})),
            ids: [
                [0, 1, 2, 3, 4],
                [1, 2, 3, 4],
                [1, 2, 3, 4],
                [1, 2, 3, 4],
                [1, 2, 3, 4],
                [2, 3, 4],
                [4],
                []
            ],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant =>
                                     ({$expr: {$gt: [new Date(datePrefix + constant), "$time"]}})),
            ids: [[], [0], [0], [0], [0], [0, 1], [0, 1, 2, 3], [0, 1, 2, 3, 4]],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant =>
                                     ({$expr: {$gte: [new Date(datePrefix + constant), "$time"]}})),
            ids: [[], [0], [0], [0], [0], [0, 1, 2], [0, 1, 2, 3], [0, 1, 2, 3, 4]],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant =>
                                     ({$expr: {$eq: [new Date(datePrefix + constant), "$time"]}})),
            ids: [[], [], [], [], [], [2], [], []],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant =>
                                     ({$expr: {$ne: [new Date(datePrefix + constant), "$time"]}})),
            ids: [
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4]
            ],
            usesBlockProcessing: true
        },

        {
            preds: constants.map(constant => ({
                                     "$expr": {
                                         "$gt": [
                                             {
                                                 "$dateDiff": {
                                                     "startDate": "$time",
                                                     "endDate": new Date(datePrefix + constant),
                                                     "unit": "millisecond"
                                                 }
                                             },
                                             0
                                         ]
                                     }
                                 })),
            ids: [[], [0], [0], [0], [0], [0, 1], [0, 1, 2, 3], [0, 1, 2, 3, 4]],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({
                                     "$expr": {
                                         "$gt": [
                                             {
                                                 "$dateDiff": {
                                                     "startDate": new Date(datePrefix + constant),
                                                     "endDate": "$time",
                                                     "unit": "millisecond"
                                                 }
                                             },
                                             -60
                                         ]
                                     }
                                 })),
            ids: [
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [0, 1, 2, 3, 4],
                [2, 3, 4],
                [3, 4],
                [4]
            ],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(
                constant => ({
                    "$expr": {
                        "$eq": [
                            {
                                "$dateAdd":
                                    {"startDate": "$time", "unit": "millisecond", amount: 100}
                            },
                            new Date(datePrefix + constant)
                        ]
                    }
                })),
            ids: [[], [], [], [], [], [1], [], []],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(
                constant => ({
                    "$expr": {
                        "$eq": [
                            {
                                "$dateSubtract":
                                    {"startDate": "$time", "unit": "millisecond", amount: constant}
                            },
                            new Date(datePrefix)
                        ]
                    }
                })),
            ids: [[], [], [], [], [], [2], [], []],
            usesBlockProcessing: true
        },

        {
            preds: constants.map(constant => ({"nestedArray.subField": constant})),
            ids: [[], [1], [1], [], [], [], [], []],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"sometimesDoublyNestedArray": constant})),
            ids: [[], [], [1], [], [], [], [], []],
            usesBlockProcessing: true
        },
        {
            preds: constants.map(constant => ({"sometimesDoublyNestedArray": [constant]})),
            ids: [[], [1], [], [], [], [], [], []],
            usesBlockProcessing: false
        },
    ];

    runTests(conn, db, coll, kTestCases);

    MongoRunner.stopMongod(conn);
});
