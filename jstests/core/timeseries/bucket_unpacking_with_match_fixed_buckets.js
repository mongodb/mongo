/**
 * Test that eventFilter and wholeBucketFilter are removed for queries in time-series collections
 * with fixed buckets.
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
 *     featureFlagTSBucketingParametersUnchanged,
 *     requires_getmore,
 * ]
 */

import {getAggPlanStages} from "jstests/libs/analyze_plan.js";

const coll = db.bucket_unpack_with_match_fixed_buckets;
const metaField = "mt";
const timeField = "time";

function checkResults({
    pipeline,
    expectedDocs,
    eventFilter = null,
    wholeBucketFilter = null,
    fixedBuckets = true,
    expectUnpackStage = true
}) {
    checkExplain(pipeline, eventFilter, wholeBucketFilter, fixedBuckets, expectUnpackStage);
    let results = coll.aggregate(pipeline).toArray();
    assert.sameMembers(results, expectedDocs);
}

function checkExplain(pipeline, eventFilter, wholeBucketFilter, fixedBuckets, expectUnpackStage) {
    const explain = coll.explain().aggregate(pipeline);
    const unpackStages = getAggPlanStages(explain, "$_internalUnpackBucket");
    if (expectUnpackStage) {
        assert.eq(1, unpackStages.length, `Expected $_internalUnpackBucket in ${tojson(explain)}`);

        // Validate that _eventFilter and wholeBucketFilter are correct.
        const unpackStage = unpackStages[0]["$_internalUnpackBucket"];
        const actualEventFilter = unpackStage["eventFilter"];
        const actualWholeBucketFilter = unpackStage["wholeBucketFilter"];

        assert.eq(actualEventFilter, eventFilter, unpackStage);
        assert.eq(actualWholeBucketFilter, wholeBucketFilter, unpackStage);
        if (fixedBuckets) {
            assert(unpackStage["fixedBuckets"], unpackStage);
        } else {
            assert(!unpackStage["fixedBuckets"], unpackStage);
        }
    } else {
        assert.eq([], unpackStages, `Expected no unpack stage in the pipeline ${explain}`);
    }
}

function testDeterministicInput(roundingParam, startingTime) {
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
    let times = [
        new Date(startingTime.getTime() - offset),
        new Date(startingTime.getTime() - offset / 2),
        new Date(startingTime.getTime() - offset / 3),
        startingTime,
        new Date(startingTime.getTime() + offset / 3),
        new Date(startingTime.getTime() + offset / 2),
        new Date(startingTime.getTime() + offset)
    ];
    times.forEach((time, index) => {
        docs.push({
            _id: index,
            [timeField]: time,
            [metaField]: {"id": 1234, "location": "nyc"},
            otherTime: time,
            accValue: index * 2
        });
    });
    assert.commandWorked(coll.insertMany(docs));

    //
    // These tests validate that the query is successfully rewritten in different $match
    // expressions.
    //

    // $lt when the predicate doesn't align with the bucket boundary, and we expect an _eventFilter.
    checkResults({
        pipeline: [{$match: {[timeField]: {$lt: times[2]}}}],
        expectedDocs: [docs[0], docs[1]],
        eventFilter: {[timeField]: {$lt: times[2]}},
        wholeBucketFilter: {[`control.max.${timeField}`]: {$lt: times[2]}}
    });

    // $lt when the predicate does align with the bucket boundary, and we don't expect an
    // _eventFilter.
    checkResults({
        pipeline: [{$match: {[timeField]: {$lt: startingTime}}}],
        expectedDocs: [docs[0], docs[1], docs[2]]
    });

    // $lte always has an eventFilter to catch edge cases.
    checkResults({
        pipeline: [{$match: {[timeField]: {$lte: startingTime}}}],
        expectedDocs: [docs[0], docs[1], docs[2], docs[3]],
        eventFilter: {[timeField]: {$lte: startingTime}},
        wholeBucketFilter: {[`control.max.${timeField}`]: {$lte: startingTime}}
    });

    // $gt always has an eventFilter to capture all of the correct results.
    checkResults({
        pipeline: [{$match: {[timeField]: {$gt: startingTime}}}],
        expectedDocs: [docs[4], docs[5], docs[6]],
        eventFilter: {[timeField]: {$gt: startingTime}},
        wholeBucketFilter: {[`control.min.${timeField}`]: {$gt: startingTime}}
    });
    checkResults({
        pipeline: [{$match: {[timeField]: {$gt: times[4]}}}],
        expectedDocs: [docs[5], docs[6]],
        eventFilter: {[timeField]: {$gt: times[4]}},
        wholeBucketFilter: {[`control.min.${timeField}`]: {$gt: times[4]}}

    });

    // $gte when the predicate does align with the bucket boundary, and we don't expect an
    // _eventFilter.
    checkResults({
        pipeline: [{$match: {[timeField]: {$gte: startingTime}}}],
        expectedDocs: [docs[3], docs[4], docs[5], docs[6]]
    });

    // $gte when the predicate doesn't align with the bucket boundary, and we expect an
    // _eventFilter.
    checkResults({
        pipeline: [{$match: {[timeField]: {$gte: times[4]}}}],
        expectedDocs: [docs[4], docs[5], docs[6]],
        eventFilter: {[timeField]: {$gte: times[4]}},
        wholeBucketFilter: {[`control.min.${timeField}`]: {$gte: times[4]}}
    });

    // $eq always has an eventFilter to capture all of the correct results.
    checkResults({
        pipeline: [{$match: {[timeField]: startingTime}}],
        expectedDocs: [docs[3]],
        eventFilter: {[timeField]: {$eq: startingTime}},
        wholeBucketFilter: {
            $and: [
                {[`control.min.${timeField}`]: {$eq: startingTime}},
                {[`control.max.${timeField}`]: {$eq: startingTime}}
            ]
        }
    });
    checkResults({
        pipeline: [{$match: {[timeField]: times[0]}}],
        expectedDocs: [docs[0]],
        eventFilter: {[timeField]: {$eq: times[0]}},
        wholeBucketFilter: {
            $and: [
                {[`control.min.${timeField}`]: {$eq: times[0]}},
                {[`control.max.${timeField}`]: {$eq: times[0]}}
            ]
        }
    });

    // $in always has an eventFilter to capture all of the correct results.
    checkResults({
        pipeline: [{$match: {[timeField]: {$in: [startingTime, times[4]]}}}],
        expectedDocs: [docs[3], docs[4]],
        eventFilter: {[timeField]: {$in: [startingTime, times[4]]}}
    });
    checkResults({
        pipeline: [{$match: {[timeField]: {$in: [startingTime, times[6]]}}}],
        expectedDocs: [docs[3], docs[6]],
        eventFilter: {[timeField]: {$in: [startingTime, times[6]]}}
    });

    // Test a $match expression that is a conjunction, with all the predicates on the 'timeField'
    // that do align with bucket boundaries.
    checkResults({
        pipeline: [{
            $match: {
                $or: [
                    {[timeField]: {$gte: times[6]}},
                    {[timeField]: {$lt: startingTime}},
                ]
            }
        }],
        expectedDocs: [docs[0], docs[1], docs[2], docs[6]],
    });

    // Test multiple $match expressions, with all the predicates on the 'timeField' that align with
    // the bucket boundaries.
    checkResults({
        pipeline: [
            {$match: {[timeField]: {$lt: startingTime}}},
            {$match: {[timeField]: {$gte: times[0]}}}
        ],
        expectedDocs: [docs[0], docs[1], docs[2]]
    });

    // Test that a $group stage can replace the unpack stage after the $match stage is rewritten.
    // The $group rewrite can occur, because we do not have an _eventFilter. Therefore, we do not
    // need to unpack the buckets to individually filter measurements, and can remove the unpack
    // stage.
    checkResults({
        pipeline: [
            {$match: {[timeField]: {$lt: startingTime}}},
            {
                $group:
                    {_id: `$${metaField}`, accmin: {$min: "$accValue"}, accmax: {$max: "$accValue"}}
            }
        ],
        expectedDocs: [{"_id": {"id": 1234, "location": "nyc"}, "accmin": 0, "accmax": 4}],
        eventFilter: null,
        wholeBucketFilter: null,
        fixedBuckets: true,
        expectUnpackStage: false
    });

    //
    // These tests validate that the query is not rewritten when the requirements are not met.
    //

    // Test a $match expression that is a conjunction, with all the predicates on the 'timeField'
    // that don't align with bucket boundaries.
    checkResults({
        pipeline: [{
            $match: {
                $or: [
                    {[timeField]: {$gte: times[5]}},
                    {[timeField]: {$lt: times[2]}},
                    {[timeField]: {$lte: times[0]}}
                ]
            }
        }],
        expectedDocs: [docs[5], docs[6], docs[1], docs[0]],
        eventFilter: {
            $or: [
                {[timeField]: {$gte: times[5]}},
                {[timeField]: {$lt: times[2]}},
                {[timeField]: {$lte: times[0]}}
            ]
        },
        wholeBucketFilter: {
            $or: [
                {[`control.min.${timeField}`]: {$gte: times[5]}},
                {[`control.max.${timeField}`]: {$lt: times[2]}},
                {[`control.max.${timeField}`]: {$lte: times[0]}}
            ]
        }
    });

    // Test multiple $match expressions, with all the predicates on the 'timeField' that don't align
    // with bucket boundaries.
    checkResults({
        pipeline: [
            {$match: {[timeField]: {$in: [startingTime, times[4]]}}},
            {$match: {[timeField]: {$lt: times[5]}}},
            {$match: {[timeField]: {$gte: times[4]}}}
        ],
        expectedDocs: [docs[4]],
        eventFilter: {
            $and: [
                {[timeField]: {$in: [startingTime, times[4]]}},
                {[timeField]: {$lt: times[5]}},
                {[timeField]: {$gte: times[4]}}
            ]
        }
    });

    // Test multiple $match expressions, where one predicate is on the 'timeField' and aligns with
    // the bucket boundaries, and the other predicate is on a different field.
    checkResults({
        pipeline:
            [{$match: {[timeField]: {$lt: startingTime}}}, {$match: {otherTime: {$gte: times[0]}}}],
        expectedDocs: [docs[0], docs[1], docs[2]],
        eventFilter: {$and: [{[timeField]: {$lt: startingTime}}, {otherTime: {$gte: times[0]}}]}
    });

    // Test that comparisons to extended range dates will return either none or all documents, with
    // neither wholeBucketFilter of eventFilter being present in the unpack stage.
    checkResults({
        pipeline: [{$match: {[timeField]: {$lt: ISODate("1969-09-03T00:00:00.000Z")}}}],
        expectedDocs: [],
    });

    checkResults({
        pipeline: [{$match: {[timeField]: {$lt: ISODate("2040-09-03T00:00:00.000Z")}}}],
        expectedDocs: docs,
    });

    // Test the fixed bucketing specific rewrites are not done if the bucketing parameters have
    // been changed. We still do the generic rewrites on the 'control' fields. This test needs to
    // run last, since collMod will permanently change the flag to false.
    assert.commandWorked(db.runCommand({
        "collMod": coll.getName(),
        "timeseries": {bucketMaxSpanSeconds: 100000, bucketRoundingSeconds: 100000}
    }));
    // This query was rewritten without 'eventFilter' earlier, but now that collection's bucketing
    // parameters have changed, the query will need an 'eventFilter'.
    checkResults({
        pipeline: [{$match: {[timeField]: {$gte: startingTime}}}],
        expectedDocs: [docs[3], docs[4], docs[5], docs[6]],
        eventFilter: {[timeField]: {$gte: startingTime}},
        wholeBucketFilter: {[`control.min.${timeField}`]: {$gte: startingTime}},
        fixedBuckets: false
    });
}

// Run the test with different rounding parameters.
testDeterministicInput(3600 /* roundingParam */,
                       ISODate("2022-09-30T15:00:00.000Z") /* startingTime */);  // 1 hour
testDeterministicInput(86400 /* roundingParam */,
                       ISODate("2022-09-30T00:00:00.000Z") /* startingTime */);  // 1 day
testDeterministicInput(60 /* roundingParam */,
                       ISODate("2022-09-30T15:10:00.000Z") /* startingTime */);  // 1 minute

function checkRandomTestResult(pipeline, shouldCheckExplain = true) {
    if (shouldCheckExplain) {
        checkExplain(pipeline,
                     null /* eventFilter */,
                     null /* wholeBucketFilter */,
                     true /* fixedBuckets */,
                     true /*expectUnpackStage */);
    }
    const results = coll.aggregate(pipeline).toArray();
    const noOptResults =
        coll.aggregate([{$_internalInhibitOptimization: {}}, pipeline[0]]).toArray();
    assert.sameMembers(results, noOptResults, "Results differ with and without the optimization.");
}

function generateRandomTimestamp() {
    const startTime = ISODate("2012-01-01T00:01:00.000Z");
    const maxTime = ISODate("2015-12-31T23:59:59.000Z");
    return new Date(Math.floor(Random.rand() * (maxTime.getTime() - startTime.getTime()) +
                               startTime.getTime()));
}

(function testRandomizedInput() {
    Random.setRandomSeed();
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {
        timeseries: {
            timeField: timeField,
            metaField: metaField,
            bucketMaxSpanSeconds: 900,
            bucketRoundingSeconds: 900
        }
    }));

    let docs = [];
    // Insert 1000 documents at random times spanning 3 years (between 2012 and 2015). These dates
    // were chosen arbitrarily.
    for (let i = 0; i < 1000; i++) {
        docs.push({[timeField]: generateRandomTimestamp(), [metaField]: "location"});
    }
    assert.commandWorked(coll.insertMany(docs));

    // Validate simple queries with the optimization return the same result as the query run without
    // the optimization.
    checkRandomTestResult([{$match: {[timeField]: {$gte: ISODate("2014-01-01T15:30:00.000Z")}}}]);
    checkRandomTestResult([{$match: {[timeField]: {$lt: ISODate("2013-01-01T18:45:00.000Z")}}}]);

    // Validate the same results are returned with a completely random timestamp. We will not check
    // the explain output, since we cannot guarantee the time will align with the bucket boundaries.
    checkRandomTestResult([{$match: {[timeField]: {$lt: generateRandomTimestamp()}}}],
                          false /* shouldCheckExplain */);
    checkRandomTestResult([{$match: {[timeField]: {$gte: generateRandomTimestamp()}}}],
                          false /* shouldCheckExplain */);
})();
