/**
 *  Test the behavior of $group on time-series collections. Specifically, we are targeting rewrites
 * that replace bucket unpacking with $group over fixed buckets. This optimization only applies if
 * the '_id' field is a combination of constant expressions, field paths referencing metaField,
 * and/or $dateTrunc expressions on the timeField.
 *
 * @tags: [
 *     # We need a timeseries collection.
 *     requires_timeseries,
 *     requires_fcv_71,
 *     # Explain of a resolved view must be executed by mongos.
 *     directly_against_shardsvrs_incompatible,
 *     # Refusing to run a test that issues an aggregation command with explain because it may
 *     # return incomplete results if interrupted by a stepdown.
 *     does_not_support_stepdowns,
 *     # The `simulate_atlas_proxy` override cannot deep copy very large or small dates.
 *     simulate_atlas_proxy_incompatible,
 *     # TODO (SERVER-88275) a moveCollection can cause the original collection to be dropped and
 *     # re-created with a different uuid, causing the aggregation to fail with QueryPlannedKilled
 *     # when the mongos is fetching data from the shard using getMore(). Remove the tag the issue
 *     # is solved
 *     assumes_balancer_off,
 *     featureFlagTSBucketingParametersUnchanged,
 * ]
 */

import {getAggPlanStage, getEngine, getPlanStage} from "jstests/libs/query/analyze_plan.js";

(function() {
"use strict";

const coll = db[jsTestName()];
// For sanity check of the results, we'll use as oracle a collection that contains the same data but
// isn't time-series.
const collNonTs = db[jsTestName() + '_non_ts'];

const timeField = "time";
const measurementField = "b";
const metaField = "mt";

// Check in explain that whether the rewrite has happened.
function checkRewrite({pipeline, rewriteExpected}) {
    const explain = coll.explain().aggregate(pipeline);
    const unpack = (getEngine(explain) === "classic")
        ? getAggPlanStage(explain, "$_internalUnpackBucket")
        : getPlanStage(explain, "UNPACK_TS_BUCKET");
    // The rewrite should remove the unpack stage and replace it with a $group over the buckets
    // collection.
    if (rewriteExpected) {
        assert(!unpack,
               `Expected to find no unpack stage for pipeline ${tojson(pipeline)} but got ${
                   tojson(explain)}`);
    } else {
        assert(unpack,
               `Expected to have the unpack stage for pipeline ${tojson(pipeline)} but got ${
                   tojson(explain)}`);
    }
}

function checkResults({pipeline, expectedResults}) {
    const results = coll.aggregate(pipeline).toArray();
    const nonTsResults = collNonTs.aggregate(pipeline).toArray();
    if (expectedResults) {
        assert.sameMembers(
            nonTsResults, expectedResults, `Results for pipeline ${tojson(pipeline)} over non-TS`);
        assert.sameMembers(results, expectedResults, `Results for pipeline ${tojson(pipeline)}`);
    } else {
        // In randomized tests we cannot provide the oracle for expected docs but we can compare
        // the results against TS and non-TS collections.
        assert.sameMembers(
            results,
            nonTsResults,
            `Results for pipeline ${tojson(pipeline)} over TS(left) vs non-TS(right) collections.`);
    }
}

let b, times = [];
function setUpSmallCollection({roundingParam, startingTime}) {
    collNonTs.drop();
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {
        timeseries: {
            timeField: timeField,
            metaField: metaField,
            bucketMaxSpanSeconds: roundingParam,
            bucketRoundingSeconds: roundingParam
        }
    }));
    let docs = [];
    // Need to convert the 'bucketRoundingSeconds' and 'bucketMaxSpanSeconds' to milliseconds.
    const offset = roundingParam * 1000;
    // Add documents that will span over multiple buckets.
    times = [
        new Date(startingTime.getTime() - offset),
        new Date(startingTime.getTime() - offset / 2),
        new Date(startingTime.getTime() - offset / 3),
        startingTime,
        new Date(startingTime.getTime() + offset / 3),
        new Date(startingTime.getTime() + offset / 2),
        new Date(startingTime.getTime() + offset)
    ];
    b = [2, 1, 4, 3, 5, 6, 7];
    times.forEach((time, index) => {
        docs.push({
            _id: index,
            [timeField]: time,
            [metaField]: "MDB",
            [measurementField]: b[index],
            "otherTime": time
        });
    });
    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(collNonTs.insertMany(docs));
}

///
// These tests will validate the group stage is rewritten when the '_id' field has a $dateTrunc
// expression.
///
const groupByDateTrunc_ExpectRewrite = [
    // Validate the rewrite occurs with a simple case, where the bucket boundary and 'unit' are the
    // same.
    {
        pipeline: [{
            $group: {
                _id: {t: {$dateTrunc: {date: `$${timeField}`, unit: "hour"}}},
                accmin: {$min: `$${measurementField}`},
                accmax: {$max: `$${measurementField}`}
            }
        }],
        expectedResults: [
            {_id: {t: ISODate("2022-09-30T15:00:00Z")}, accmin: 3, accmax: 6},
            {_id: {t: ISODate("2022-09-30T16:00:00Z")}, accmin: 7, accmax: 7},
            {_id: {t: ISODate("2022-09-30T14:00:00Z")}, accmin: 1, accmax: 4}
        ]
    },

    // Validate the rewrite occurs with all the optional fields present.
    {
        pipeline: [{
            $group: {
                _id: {
                    t: {
                        $dateTrunc: {
                            date: `$${timeField}`,
                            unit: "day",
                            timezone: "+0500",
                            binSize: 2,
                            startOfWeek: "friday"
                        }
                    }
                },
                accmin: {$min: `$${measurementField}`},
                accmax: {$max: `$${measurementField}`}
            }
        }],
        expectedResults: [{_id: {t: ISODate("2022-09-29T19:00:00Z")}, accmin: 1, accmax: 7}],
    },

    // Validate the rewrite occurs with multiple expressions in the '_id' field.
    {
        pipeline: [{
            $group: {
                _id: {
                    constant: "hello",
                    m: `$${metaField}`,
                    t: {$dateTrunc: {date: `$${timeField}`, unit: "day"}}
                },
                accmin: {$min: `$${measurementField}`},
                accmax: {$max: `$${measurementField}`}
            }
        }],
        expectedResults: [{
            _id: {t: ISODate("2022-09-30T00:00:00Z"), m: "MDB", constant: "hello"},
            accmin: 1,
            accmax: 7
        }],
    },

    // Validate the rewrite occurs with a timezone with the same hourly boundaries, and
    // bucketMaxSpanSeconds == 3600.
    {
        pipeline: [{
            $group: {
                _id: {
                    m: `$${metaField}`,
                    t: {$dateTrunc: {date: `$${timeField}`, unit: "day", timezone: "+0800"}}
                },
                accmin: {$min: `$${measurementField}`},
                accmax: {$max: `$${measurementField}`}
            }
        }],
        expectedResults: [
            {_id: {"m": "MDB", t: ISODate("2022-09-29T16:00:00Z")}, accmin: 1, accmax: 6},
            {_id: {"m": "MDB", t: ISODate("2022-09-30T16:00:00Z")}, accmin: 7, accmax: 7}
        ],
    },

    // The 'unit' field in $dateTrunc is larger than 'week', but 'bucketMaxSpanSeconds' is less than
    // 1 day. The rewrite applies.
    {
        pipeline: [{
            $group: {
                _id: {t: {$dateTrunc: {date: `$${timeField}`, unit: "year"}}},
                accmin: {$min: `$${measurementField}`},
                accmax: {$max: `$${measurementField}`}
            }
        }],
        expectedResults: [{_id: {t: ISODate("2022-01-01T00:00:00Z")}, accmin: 1, accmax: 7}],
    },

    // Validate the rewrite occurs with the $count accumulator.
    {
        pipeline: [{
            $group: {
                _id: {c: "string", t: {$dateTrunc: {date: `$${timeField}`, unit: "month"}}},
                accmin: {$min: `$${measurementField}`},
                accmax: {$max: `$${measurementField}`},
                count: {$count: {}},
            }
        }],
        expectedResults: [{
            _id: {"c": "string", t: ISODate("2022-09-01T00:00:00Z")},
            accmin: 1,
            accmax: 7,
            count: 7
        }],
    }
];
(function testGroupByDateTrunct_ExpectRewrite() {
    setUpSmallCollection({roundingParam: 3600, startingTime: ISODate("2022-09-30T15:00:00.000Z")});

    groupByDateTrunc_ExpectRewrite.forEach(testCase => {
        checkRewrite({pipeline: testCase.pipeline, rewriteExpected: true});
        checkResults(testCase);
    });
})();

///
// These tests will validate the optimization did not occur.
///
const groupByDateTrunc_NoRewrite = [
    // There is a timezone with different hourly boundaries that causes the boundaries to not align.
    // Asia/Kathmandu has a UTC offset of +05:45.
    {
        pipeline: [{
            $group: {
                _id: {
                    t: {
                        $dateTrunc: {
                            date: `$${timeField}`,
                            unit: "hour",
                            binSize: 24,
                            timezone: "Asia/Kathmandu"
                        }
                    }
                },
                accmin: {$min: `$${measurementField}`},
                accmax: {$max: `$${measurementField}`}
            }
        }],
        expectedResults: [{_id: {t: ISODate("2022-09-29T18:15:00Z")}, accmin: b[1], accmax: b[6]}],
    },

    // The $dateTrunc expression doesn't align with bucket boundaries.
    {
        pipeline: [{
            $group: {
                _id: {t: {$dateTrunc: {date: `$${timeField}`, unit: "second"}}},
                accmin: {$min: `$${measurementField}`},
                accmax: {$max: `$${measurementField}`},
            }
        }],
        expectedResults: [
            {_id: {t: times[0]}, accmin: b[0], accmax: b[0]},
            {_id: {t: times[1]}, accmin: b[1], accmax: b[1]},
            {_id: {t: times[2]}, accmin: b[2], accmax: b[2]},
            {_id: {t: times[3]}, accmin: b[3], accmax: b[3]},
            {_id: {t: times[4]}, accmin: b[4], accmax: b[4]},
            {_id: {t: times[5]}, accmin: b[5], accmax: b[5]},
            {_id: {t: times[6]}, accmin: b[6], accmax: b[6]}
        ],
    },

    // The $dateTrunc expression is not on the timeField.
    {
        pipeline: [{
            $group: {
                _id: {t: {$dateTrunc: {date: "$otherTime", unit: "day"}}},
                accmin: {$min: `$${measurementField}`},
                accmax: {$max: `$${measurementField}`},
            }
        }],
        expectedResults: [{_id: {t: ISODate("2022-09-30T00:00:00Z")}, accmax: 7, accmin: 1}],
    },

    // There are other expressions in the '_id' field that are not on the meta nor time fields.
    {
        pipeline: [{
            $group: {
                _id: {m: `$${metaField}`, t: {$dateTrunc: {date: "$otherTime", unit: "day"}}},
                accmax: {$max: `$${measurementField}`},
            }
        }],
        expectedResults: [{_id: {"m": "MDB", t: ISODate("2022-09-30T00:00:00Z")}, accmax: 7}],
    },

    // The fields in the $dateTrunc expression are not constant.
    {
        pipeline: [{
            $group: {
                _id: {t: {$dateTrunc: {date: `$${timeField}`, unit: "hour", binSize: "$a"}}},
                accmax: {$max: `$${measurementField}`},
                accmin: {$min: `$${measurementField}`},
            }
        }],
        expectedResults: [{_id: {t: null}, accmax: 7, accmin: 1}],
    },
];
(function testGroupByDateTrunct_NoRewrite() {
    setUpSmallCollection({roundingParam: 3600, startingTime: ISODate("2022-09-30T15:00:00.000Z")});

    groupByDateTrunc_NoRewrite.forEach(testCase => {
        checkRewrite({pipeline: testCase.pipeline, rewriteExpected: false});
        checkResults(testCase);
    });
})();

(function testCollMod() {
    setUpSmallCollection({roundingParam: 3600, startingTime: ISODate("2022-09-30T15:00:00.000Z")});
    // The parameters have changed, and thus the buckets are not fixed.
    assert.commandWorked(db.runCommand({
        "collMod": coll.getName(),
        "timeseries": {bucketMaxSpanSeconds: 100000, bucketRoundingSeconds: 100000}
    }));

    const pipeline = [{
        $group: {
            _id: {t: {$dateTrunc: {date: `$${timeField}`, unit: "day"}}},
            accmin: {$min: `$${measurementField}`},
            accmax: {$max: `$${measurementField}`}
        }
    }];
    checkResults({
        pipeline: pipeline,
        expectedResults: [{_id: {t: ISODate("2022-09-30T00:00:00Z")}, accmin: 1, accmax: 7}],
    });
    checkRewrite({pipeline: pipeline, rewriteExpected: false});
})();

// Validate the rewrite does not apply for fixed buckets with a 'bucketMaxSpanSeconds' set to
// greater than one day. This is because the bucket rounding logic and $dateTrunc rounding is
// different and becomes too unreliable.
(function testLargeBucketSpan() {
    const secondsInTwoDays = 3600 * 48;
    setUpSmallCollection(
        {roundingParam: secondsInTwoDays, startingTime: ISODate("2012-06-30T23:00:00.000Z")});
    const timeUnits = ["minute", "second", "day", "week", "year"];
    timeUnits.forEach(timeUnit => {
        const pipeline = [{
            $group: {
                _id: {t: {$dateTrunc: {date: `$${timeField}`, unit: timeUnit}}},
                accmin: {$min: `$${measurementField}`},
                accmax: {$max: `$${measurementField}`}
            }
        }];
        checkResults({pipeline: pipeline});
        checkRewrite({pipeline: pipeline, rewriteExpected: false});
    });
})();

// Validate the results with and without the optimization are the same with a random
// bucketMaxSpanSeconds. bucketMaxSpanSeconds can be any integer between 1-31536000 inclusive.
// This test doesn't check for rewrite because it would depend on the chosen value for the span.
(function testRandomBucketSpan() {
    const seedVal = new Date().getTime();
    Random.setRandomSeed(seedVal);
    const randomSpan = Math.floor(Random.rand() * (31536000 - 1) + 1);
    jsTestLog(`In testRandomBucketSpan using randomSpan: ${randomSpan}`);

    setUpSmallCollection(
        {roundingParam: randomSpan, startingTime: ISODate("2015-06-26T23:00:00.000Z")});
    const timeUnits = ["millisecond", "minute", "second", "day", "week", "month", "year"];
    timeUnits.forEach(timeUnit => {
        checkResults({
            pipeline: [{
                $group: {
                    _id: {t: {$dateTrunc: {date: `$${timeField}`, unit: timeUnit}}},
                    accmin: {$min: `$${measurementField}`},
                    accmax: {$max: `$${measurementField}`}
                }
            }],
        });
    });
})();

// Validate the rewrite works for a smaller fixed bucketing parameter and accounts for leap seconds.
// A leap second occurred on 2012-06-30:23:59:60. $dateTrunc and time-series rounding logic rounds
// this time to the next minute.
(function testLeapSeconds() {
    setUpSmallCollection({roundingParam: 60, startingTime: ISODate("2012-06-30T23:00:00.000Z")});
    // Insert documents close and at the leap second. These numbers are larger and smaller than the
    // originally inserted documents, so they should change the values of "$min" and "$max".
    const leapSecondDocs = [
        {[timeField]: ISODate("2012-06-30T23:59:60.000Z"), [metaField]: "MDB", b: 16},
        {[timeField]: ISODate("2012-06-30T23:59:40.000Z"), [metaField]: "MDB", b: 11},
        {[timeField]: ISODate("2012-06-30T23:59:45.000Z"), [metaField]: "MDB", B: 12},
        {[timeField]: ISODate("2012-06-30T23:59:50.000Z"), [metaField]: "MDB", b: -1},
        {[timeField]: ISODate("2012-06-30T23:59:59.000Z"), [metaField]: "MDB", b: 0},
        {[timeField]: ISODate("2012-07-01T00:00:05.000Z"), [metaField]: "MDB", b: 15}
    ];
    assert.commandWorked(coll.insertMany(leapSecondDocs));
    assert.commandWorked(collNonTs.insertMany(leapSecondDocs));

    const pipeline = [{
        $group: {
            _id: {t: {$dateTrunc: {date: `$${timeField}`, unit: "minute"}}},
            accmin: {$min: `$${measurementField}`},
            accmax: {$max: `$${measurementField}`}
        }
    }];
    checkResults({
        pipeline: pipeline,
        expectedDocs: [
            {_id: {t: ISODate("2012-06-30T23:59:00Z")}, accmin: -1, accmax: 11},
            {_id: {t: ISODate("2012-06-30T22:59:00Z")}, accmin: 1, accmax: 4},
            {_id: {t: ISODate("2012-07-01T00:00:00Z")}, accmin: 15, accmax: 16},
            {_id: {t: ISODate("2012-06-30T23:01:00Z")}, accmin: 7, accmax: 7},
            {_id: {t: ISODate("2012-06-30T23:00:00Z")}, accmin: 3, accmax: 6},
        ],
    });
    checkRewrite({pipeline: pipeline, rewriteExpected: true});
})();

// Validate the rewrite works for daylight savings. Due to daylight savings March 13, 2022
// was 23 hours long, since the hour between 2-3:00am was skipped. We will be testing the New York
// timezone, so 2:00 for New York in UTC is 7:00.
(function testDaylightSavings() {
    setUpSmallCollection({roundingParam: 3600, startingTime: ISODate("2022-03-13T07:00:00.000Z")});
    // Insert documents for every hour of the day in the New York timezone, even though the day was
    // only 23 hours long.   Two hours after "startTime", will be the skipped hour, but we expect
    // that document to still be valid and exist. To double check that document will have the
    // minimum value.
    const startTime = ISODate("2022-03-13T05:30:00.000Z");
    let dayLightDocs = [];
    for (let i = 0; i < 23; i++) {
        const accValue = i == 2 ? -1 : i + 8;  // set the "skipped" hour to the minimum value.
        const newTime = new Date(startTime.getTime() + (1000 * i * 60));  // i hours in the future.
        dayLightDocs.push({
            [timeField]: newTime,
            [metaField]: 1,
            [measurementField]: accValue  // avoid duplicates 'b' values in the original set.
        });
    }
    assert.commandWorked(coll.insertMany(dayLightDocs));
    assert.commandWorked(collNonTs.insertMany(dayLightDocs));

    const pipeline = [{
        $group: {
            _id: {
                t: {
                    $dateTrunc: {
                        date: `$${timeField}`,
                        unit: "hour",
                        binSize: 24,
                        timezone: "America/New_York"
                    }
                }
            },
            accmin: {$min: `$${measurementField}`},
            accmax: {$max: `$${measurementField}`}
        }
    }];
    checkResults({
        pipeline: pipeline,
        expectedResults: [{_id: {t: ISODate("2022-03-13T05:00:00Z")}, accmin: -1, accmax: 30}],
    });
    checkRewrite({pipeline: pipeline, rewriteExpected: true});
})();

// Validate a few simple queries with a randomized larger dataset return the same results with and
// without the optimization.
(function testRandomizedInput() {
    const seedVal = new Date().getTime();
    jsTestLog("In testRandomizedInput using seed value: " + seedVal);
    Random.setRandomSeed(seedVal);
    coll.drop();
    collNonTs.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {
        timeseries: {
            timeField: timeField,
            metaField: metaField,
            bucketMaxSpanSeconds: 86400,
            bucketRoundingSeconds: 86400
        }
    }));

    let docs = [];
    const startTime = ISODate("2012-01-01T00:01:00.000Z");
    const maxTime = ISODate("2015-12-31T23:59:59.000Z");
    // Insert 1000 documents at random times spanning 3 years (between 2012 and 2015). These dates
    // were chosen arbitrarily.
    for (let i = 0; i < 1000; i++) {
        const randomTime = new Date(Math.floor(
            Random.rand() * (maxTime.getTime() - startTime.getTime()) + startTime.getTime()));
        docs.push({[timeField]: randomTime, [metaField]: "location"});
    }
    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(collNonTs.insertMany(docs));

    const timeUnits = ["day", "week", "month", "quarter", "year"];
    timeUnits.forEach(timeUnit => {
        const pipeline = [{
            $group: {
                _id: {t: {$dateTrunc: {date: `$${timeField}`, unit: timeUnit}}},
                accmin: {$min: `$${measurementField}`},
                accmax: {$max: `$${measurementField}`}
            }
        }];
        checkResults({pipeline: pipeline});
        checkRewrite({pipeline: pipeline, rewriteExpected: true});
    });
})();
}());
