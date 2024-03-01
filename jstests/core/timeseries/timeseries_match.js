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
        {pred: {"topLevelScalar": {$eq: [999, 999]}}, ids: [], usesBlockProcessing: false},

        // These tests intentionally use nested arrays to force $in to produce Nothing values.
        {
            pred: {"time": {$in: [[new Date("2019-09-27T21:14:45.654Z")]]}},
            ids: [],
            usesBlockProcessing: true
        },
        {
            pred: {
                "time": {
                    $not:
                        {$in: [[new Date("2019-09-27T21:14:45.654Z"), new Date(datePrefix + 300)]]}
                }
            },
            ids: [0, 1, 2, 3, 4],
            usesBlockProcessing: true
        },

        // Basic support for boolean operators.
        {
            pred: {
                $or: [
                    {
                        $and: [
                            {"time": {$gte: new Date("2019-09-27T21:14:45.654Z")}},
                            {"time": {$gt: new Date(datePrefix + 300)}}
                        ]
                    },
                    {"time": {$eq: new Date(datePrefix + 300)}}
                ]
            },
            ids: [2],
            usesBlockProcessing: true
        },
        // Test boolean operators dealing with Nothing values.
        {
            pred: {
                $nor: [
                    {"time": {$ne: ["arr1", "arr2"]}},
                    {
                        $and: [
                            {"time": {$gte: ["arr3", "arr4"]}},
                            {"time": {$gt: new Date(datePrefix + 300)}}
                        ]
                    },
                    {"time": {$eq: new Date(datePrefix + 300)}}
                ]
            },
            ids: [],
            usesBlockProcessing: true
        },
        // Logical operators between scalar and block values.
        {
            pred: {
                $or: [
                    {$expr: {$regexFind: {input: "$measurement", regex: "^2", options: ""}}},
                    {"topLevelScalar": {$lte: 200}}
                ]
            },
            ids: [0],
            usesBlockProcessing: false
        },
        {pred: {$expr: {$lt: [101, "$topLevelScalar"]}}, ids: [0, 1], usesBlockProcessing: true},
        {
            pred: {$expr: {$lt: [new Date(datePrefix + 300), "$time"]}},
            ids: [3, 4],
            usesBlockProcessing: true
        },
        {
            pred: {$expr: {$lte: [new Date(datePrefix + 300), "$time"]}},
            ids: [2, 3, 4],
            usesBlockProcessing: true
        },
        {
            pred: {$expr: {$gt: [new Date(datePrefix + 300), "$time"]}},
            ids: [0, 1],
            usesBlockProcessing: true
        },
        {
            pred: {$expr: {$gte: [new Date(datePrefix + 300), "$time"]}},
            ids: [0, 1, 2],
            usesBlockProcessing: true
        },
        {
            pred: {$expr: {$eq: [new Date(datePrefix + 300), "$time"]}},
            ids: [2],
            usesBlockProcessing: true
        },
        {
            pred: {$expr: {$ne: [new Date(datePrefix + 300), "$time"]}},
            ids: [0, 1, 3, 4],
            usesBlockProcessing: true
        },

        {
            pred: {
                "$expr": {
                    "$gt": [
                        {
                            "$dateDiff": {
                                "startDate": "$time",
                                "endDate": new Date(datePrefix + 150),
                                "unit": "millisecond"
                            }
                        },
                        0
                    ]
                }
            },
            ids: [0],
            usesBlockProcessing: true
        },
        {
            pred: {
                "$expr": {
                    "$gt": [
                        {
                            "$dateDiff": {
                                "startDate": new Date(datePrefix + 550),
                                "endDate": "$time",
                                "unit": "millisecond"
                            }
                        },
                        -60
                    ]
                }
            },
            ids: [4],
            usesBlockProcessing: true
        },
        {
            pred: {
                "$expr": {
                    "$eq": [
                        {"$dateAdd": {"startDate": "$time", "unit": "millisecond", amount: 100}},
                        new Date(datePrefix + 600)
                    ]
                }
            },
            ids: [4],
            usesBlockProcessing: true
        },
        {
            pred: {
                "$expr": {
                    "$eq": [
                        {
                            "$dateSubtract":
                                {"startDate": "$time", "unit": "millisecond", amount: 100}
                        },
                        new Date(datePrefix)
                    ]
                }
            },
            ids: [0],
            usesBlockProcessing: true
        },

        // Comparisons with an empty array.
        {pred: {"emptyArray": {$exists: true}}, ids: [0, 1], usesBlockProcessing: true},
        {pred: {"emptyArray": {$exists: false}}, ids: [2, 3, 4], usesBlockProcessing: true},
        {pred: {"emptyArray": null}, ids: [2, 3, 4], usesBlockProcessing: false},
        {pred: {"emptyArray": []}, ids: [0, 1], usesBlockProcessing: false},
        {pred: {"emptyArray": "foobar"}, ids: [], usesBlockProcessing: true},
        {pred: {"emptyArray": {$type: "array"}}, ids: [0, 1], usesBlockProcessing: true},
        // Case where there's a predicate which always returns the same value.
        {pred: {"emptyArray": {$lt: NaN}}, ids: [], usesBlockProcessing: true},

        {pred: {"nestedArray": {$exists: true}}, ids: [0, 1, 4], usesBlockProcessing: true},
        {pred: {"nestedArray": {$exists: false}}, ids: [2, 3], usesBlockProcessing: true},
        {pred: {"nestedArray": null}, ids: [2, 3], usesBlockProcessing: false},
        {pred: {"nestedArray": []}, ids: [], usesBlockProcessing: false},
        {pred: {"nestedArray": "foobar"}, ids: [], usesBlockProcessing: true},
        {pred: {"nestedArray": {$type: "array"}}, ids: [0, 1, 4], usesBlockProcessing: true},

        {
            pred: {"nestedArray.subField": {$exists: true}},
            ids: [0, 1, 4],
            usesBlockProcessing: false
        },
        {pred: {"nestedArray.subField": {$exists: false}}, ids: [2, 3], usesBlockProcessing: false},
        {pred: {"nestedArray.subField": null}, ids: [2, 3], usesBlockProcessing: false},
        {pred: {"nestedArray.subField": []}, ids: [4], usesBlockProcessing: false},
        {pred: {"nestedArray.subField": 103}, ids: [1], usesBlockProcessing: true},
        {pred: {"nestedArray.subField": 101}, ids: [1], usesBlockProcessing: true},
        {pred: {"nestedArray.subField": 1}, ids: [0], usesBlockProcessing: true},
        {
            pred: {"nestedArray.subField": {$type: "array"}},
            ids: [0, 1, 4],
            usesBlockProcessing: false
        },

        {
            pred: {"sometimesDoublyNestedArray": {$exists: true}},
            ids: [0, 1, 4],
            usesBlockProcessing: true
        },
        {
            pred: {"sometimesDoublyNestedArray": {$exists: false}},
            ids: [2, 3],
            usesBlockProcessing: true
        },
        {pred: {"sometimesDoublyNestedArray": null}, ids: [2, 3], usesBlockProcessing: false},
        {pred: {"sometimesDoublyNestedArray": []}, ids: [0, 4], usesBlockProcessing: false},
        {pred: {"sometimesDoublyNestedArray": 102}, ids: [1], usesBlockProcessing: true},
        // 101 is within a nested array, so it does not match. However, searching for [101] does
        // match.
        {pred: {"sometimesDoublyNestedArray": 101}, ids: [], usesBlockProcessing: true},
        {pred: {"sometimesDoublyNestedArray": [101]}, ids: [1], usesBlockProcessing: false},
        {pred: {"sometimesDoublyNestedArray": 102}, ids: [1], usesBlockProcessing: true},
        {
            pred: {"sometimesDoublyNestedArray": {$type: "array"}},
            ids: [0, 1, 4],
            usesBlockProcessing: true
        },
        {pred: {"sometimesDoublyNestedArray": [101]}, ids: [1], usesBlockProcessing: false},
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
        function testCaseAndExplainFn(description) {
            return () => description + " for test case " + tojson(testCase) +
                " failed with explain " + tojson(singleNodeQueryPlanner);
        }

        if (sbeEnabled || singleNodeQueryPlanner.winningPlan.slotBasedPlan) {
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
        assert.eq(res.length, coll.count(), res);
    }

    // Make sure that the bitmap from the previous stage is forwarded to the next stage
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName, metaField: metaFieldName},
    }));
    assert.contains(bucketsColl.getName(), db.getCollectionNames())

    insert(
        coll,
        {_id: 0, [timeFieldName]: new Date(datePrefix + 100), [metaFieldName]: "cpu", a: 1, b: 1});
    insert(
        coll,
        {_id: 0, [timeFieldName]: new Date(datePrefix + 200), [metaFieldName]: "cpu", a: 2, b: 0});
    insert(
        coll,
        {_id: 0, [timeFieldName]: new Date(datePrefix + 300), [metaFieldName]: "cpu", a: 1, b: 3});

    {
        const pipeline = [
            {$addFields: {ex: 3}},
            {$match: {a: {$eq: 1}}},
            {$match: {$expr: {$lt: ["$b", "$ex"]}}},
            {
                $group: {
                    "_id": {"time": {"$dateTrunc": {"date": "$time", "unit": "minute"}}},
                    "suma": {"$sum": "$a"}
                }
            }
        ];

        const res = coll.aggregate(pipeline).toArray();
        assert.docEq([{"_id": {"time": ISODate("1970-01-20T10:55:00Z")}, "suma": 1}], res);
    }
});
