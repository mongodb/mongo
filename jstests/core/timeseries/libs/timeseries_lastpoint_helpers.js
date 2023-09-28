/**
 * Helpers for testing lastpoint queries on time-series collections.
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {TimeseriesAggTests} from "jstests/core/timeseries/libs/timeseries_agg_helpers.js";
import {getAggPlanStage} from "jstests/libs/analyze_plan.js";

// These are functions instead of const variables to avoid tripping up the parallel jstests.
export function getEquivalentStrings() {
    return ['a', 'A', 'b', 'B'];
}

export function getEquivalentNumbers() {
    return [7, NumberInt(7), NumberLong(7), NumberDecimal(7)];
}

export function verifyLastpoint({tsColl, observerColl, pipeline, precedingFilter, expectStage}) {
    jsTestLog(`Verifying last point for indexes: ${tojson(tsColl.getIndexes())} and pipeline: ${
        tojson(pipeline)}`);

    // Verify lastpoint optmization.
    const explain = tsColl.explain().aggregate(pipeline);
    expectStage({explain, precedingFilter});

    // Assert that the time-series aggregation results match that of the observer collection.
    const expected = observerColl.aggregate(pipeline).toArray();
    const actual = tsColl.aggregate(pipeline).toArray();
    assertArrayEq({actual, expected});
}

export function createBoringCollections(includeIdleMeasurements = false) {
    // Prepare collections. Note: we usually test without idle measurements (all meta subfields are
    // non-null). If we allow the insertion of idle measurements, we will obtain multiple lastpoints
    // per bucket, and may have different results on the observer and timeseries collections.
    const numHosts = 10;
    const numIterations = 20;
    return TimeseriesAggTests.prepareInputCollections(
        numHosts, numIterations, includeIdleMeasurements);
}

// Generate interesting values.
export function generateInterestingValues() {
    const epoch = ISODate('1970-01-01');

    // Pick values with interesting equality behavior.
    let values = [
        // Arrays whose highest or lowest element is equal.
        [5],
        [5, 99],
        [99],
        // Objects that differ only by field order.
        {x: 1, y: 2},
        {y: 2, x: 1},
        // A variety of values that are "empty" somehow.
        // Missing can't be represented as a JS value--handled later.
        null,
        // Undefined is not supported:
        // "The $_internalUnpackBucket stage allows metadata to be absent or otherwise, it must
        // not be the deprecated undefined bson type", code 5369600.
        undefined,
        [],
        {},
        "",
    ];

    // Test strings that differ only by case and numbers that differ only by type.
    values = values.concat(getEquivalentStrings()).concat(getEquivalentNumbers());

    // Also wrap each interesting value in an object or array.
    // Some values that are "equal" at the top level may be distinguished when wrapped this way.
    const arrayWrapped = values.map(v => [v]);
    const objectWrapped = values.map(v => ({w: v}));
    values = values.concat(arrayWrapped).concat(objectWrapped);

    let docs = [];
    // Each event's _id is an autoincrementing number, and its timestamp is epoch + _id.
    // Run through the interesting values twice to ensure each one has two events.
    // Do this in the outer loop to ensure all the intervals overlap.
    for (const _ of [1, 2]) {
        for (const m of values) {
            docs.push({_id: docs.length, tags: {hostid: m}, time: new Date(+epoch + docs.length)});
        }
        // Handle 'missing' metaField.
        docs.push({_id: docs.length, time: new Date(+epoch + docs.length)});
    }

    for (const m of values) {
        // Push a second measurement an hour later to create another bucket for this meta field.
        docs.push({
            _id: docs.length,
            tags: {hostid: m},
            time: new Date(+epoch + docs.length + 60 * 60 * 1000)
        });
    }

    return docs;
}

export function getMapInterestingValuesToEquivalentsStage() {
    const firstElemInId = {$arrayElemAt: ["$_id", 0]};
    const isIdArray = {$isArray: "$_id"};
    const equivalentStrings = getEquivalentStrings();
    const equivalentNumbers = getEquivalentNumbers();
    return {
        $addFields: {
            _id: {
                $switch: {
                    branches: [
                        // Replace equivalent string cases with their lowercase counterparts.
                        {
                            case: {$in: ["$_id.w", equivalentStrings]},
                            then: {w: {$toLower: "$_id.w"}}
                        },
                        {
                            case: {$and: [isIdArray, {$in: [firstElemInId, equivalentStrings]}]},
                            then: [{$toLower: firstElemInId}]
                        },
                        {
                            case: {$and: [{$not: isIdArray}, {$in: ["$_id", equivalentStrings]}]},
                            then: {$toLower: "$_id"}
                        },
                        // Replace equal numbers with different numeric types with an int.
                        {case: {$in: ["$_id.w", equivalentNumbers]}, then: {w: 7}},
                        {
                            case: {$and: [isIdArray, {$in: [firstElemInId, equivalentNumbers]}]},
                            then: [7]
                        },
                        {
                            case: {$and: [{$not: isIdArray}, {$in: ["$_id", equivalentNumbers]}]},
                            then: 7
                        },
                    ],
                    default: "$_id"
                }
            }
        }
    };
}

export function createInterestingCollections() {
    const testDB = TimeseriesAggTests.getTestDb();
    const collation = {locale: 'en_US', strength: 2};

    // Prepare timeseries collection.
    const tsCollName = "in";
    assert.commandWorked(testDB.createCollection(
        tsCollName, {timeseries: {timeField: "time", metaField: "tags"}, collation}));
    const tsColl = testDB[tsCollName];

    const interestingValues = generateInterestingValues();
    assert.commandWorked(tsColl.insertMany(interestingValues, {ordered: false}));

    // Prepare observer collection.
    const observerCollName = "observer_in";
    assert.commandWorked(testDB.createCollection(observerCollName, {collation}));
    const observerColl = testDB[observerCollName];

    // We can't just insert the values directly, because bucketing treats "interesting" metaField
    // values differently than a regular collection would. For example, a true timeseries collection
    // would treat objects that only differ by field order as being equivalent meta values. For the
    // purposes of this test we don't care about the semantic difference between timeseries
    // collection bucketing and regular collections, only about the accuracy of the lastpoint
    // rewrite.
    assert.commandWorked(observerColl.insertMany(tsColl.find().toArray(), {ordered: false}));

    return [tsColl, observerColl];
}

export function expectDistinctScan({explain}) {
    // The query can utilize DISTINCT_SCAN.
    assert.neq(getAggPlanStage(explain, "DISTINCT_SCAN"), null, explain);

    // Pipelines that use the DISTINCT_SCAN optimization should not also have a blocking sort.
    assert.eq(getAggPlanStage(explain, "SORT"), null, explain);
}

export function expectCollScan({explain, precedingFilter}) {
    // At the bottom, there should be a COLLSCAN.
    const collScanStage = getAggPlanStage(explain, "COLLSCAN");
    assert.neq(collScanStage, null, explain);
    if (precedingFilter) {
        assert.eq(precedingFilter, collScanStage.filter, collScanStage);
    }
}

export function expectIxscan({explain, noSortInCursor}) {
    if (noSortInCursor) {
        // We can rely on the index without a cursor $sort.
        assert.eq(getAggPlanStage(explain, "SORT"), null, explain);
    } else {
        // $sort can be pushed into the cursor layer.
        assert.neq(getAggPlanStage(explain, "SORT"), null, explain);
    }

    // At the bottom, there should be a IXSCAN.
    assert.neq(getAggPlanStage(explain, "IXSCAN"), null, explain);
}

/**
    Test cases:
     1. Lastpoint queries on indexes with descending time and $first/$top (DISTINCT_SCAN).
     2. Lastpoint queries on indexes with ascending time and $last/$bottom (no DISTINCT_SCAN).
     3. Lastpoint queries on indexes with ascending time and $last/$bottom and an additional
   secondary index so that we can use the DISTINCT_SCAN optimization.
*/
export function testAllTimeMetaDirections(tsColl, observerColl, getTestCases) {
    const testDB = TimeseriesAggTests.getTestDb();
    const testCases = [
        {time: -1, useBucketsIndex: false},
        {time: 1, useBucketsIndex: false},
        {time: 1, useBucketsIndex: true}
    ];

    for (const {time, useBucketsIndex} of testCases) {
        const isTimeDescending = time < 0;
        const canUseDistinct = isTimeDescending || useBucketsIndex;

        // Test both directions of the metaField sort for each direction of time.
        for (const metaDir of [1, -1]) {
            const index = {"tags.hostid": metaDir, time};
            const bucketsIndex = useBucketsIndex
                ? {"meta.hostid": metaDir, "control.max.time": 1, "control.min.time": 1}
                : undefined;

            const tests = getTestCases({
                canUseDistinct,
                canSortOnTimeUseDistinct: (metaDir > 0) && (isTimeDescending || useBucketsIndex),
                time,
                index
            });

            // Run all tests without an index.
            for (const {pipeline, expectStageNoIndex, precedingFilter} of tests) {
                // Normally we expect to see a COLLSCAN with a SORT pushed into the cursor, but some
                // test-cases may override this.
                const expectStage = expectStageNoIndex || expectCollScan;
                verifyLastpoint({tsColl, observerColl, pipeline, precedingFilter, expectStage});
            }

            // Create index on the timeseries collection.
            const ixName = "tsIndex_time_" + time + "_meta_" + metaDir;
            tsColl.createIndex(index, {name: ixName});

            // Create an additional secondary index directly on the buckets collection so that we
            // can test the DISTINCT_SCAN optimization when time is sorted in ascending order.
            const bucketsColl = testDB["system.buckets.in"];
            const bucketsIxName = "bucketsIndex_time_" + time + "_meta_" + metaDir;
            if (bucketsIndex) {
                bucketsColl.createIndex(bucketsIndex, {name: bucketsIxName});
            }

            // Re-run all tests with an index.
            for (const {pipeline, expectStageWithIndex, precedingFilter} of tests) {
                verifyLastpoint({
                    tsColl,
                    observerColl,
                    pipeline,
                    precedingFilter,
                    expectStage: expectStageWithIndex
                });
            }

            // Drop indexes for next test.
            tsColl.dropIndex(ixName);
            if (bucketsIndex) {
                bucketsColl.dropIndex(bucketsIxName);
            }
        }
    }

    // Drop collections at the end of the test.
    tsColl.drop();
    observerColl.drop();
}
