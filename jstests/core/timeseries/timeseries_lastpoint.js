/**
 * Tests the optimization of "lastpoint"-type queries on time-series collections.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_timeseries,
 *   requires_pipeline_optimization,
 *   requires_fcv_53,
 *   # TODO (SERVER-63590): Investigate presence of getmore tag in timeseries jstests.
 *   requires_getmore,
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");
load("jstests/core/timeseries/libs/timeseries_agg_helpers.js");
load('jstests/libs/analyze_plan.js');

const testDB = TimeseriesAggTests.getTestDb();
assert.commandWorked(testDB.dropDatabase());

// Do not run the rest of the tests if the lastpoint optimization is disabled.
const getLastpointParam = db.adminCommand({getParameter: 1, featureFlagLastPointQuery: 1});
const isLastpointEnabled = getLastpointParam.hasOwnProperty("featureFlagLastPointQuery") &&
    getLastpointParam.featureFlagLastPointQuery.value;
if (!isLastpointEnabled) {
    return;
}

// Timeseries test parameters.
const numHosts = 10;
const numIterations = 20;

function verifyLastpoint({tsColl, observerColl, pipeline, precedingFilter, expectStage}) {
    // Verify lastpoint optmization.
    const explain = tsColl.explain().aggregate(pipeline);
    expectStage({explain, precedingFilter});

    // Assert that the time-series aggregation results match that of the observer collection.
    const expected = observerColl.aggregate(pipeline).toArray();
    const actual = tsColl.aggregate(pipeline).toArray();
    assertArrayEq({actual, expected});
}

function createBoringCollections(includeIdleMeasurements = false) {
    // Prepare collections. Note: we usually test without idle measurements (all meta subfields are
    // non-null). If we allow the insertion of idle measurements, we will obtain multiple lastpoints
    // per bucket, and may have different results on the observer and timeseries collections.
    return TimeseriesAggTests.prepareInputCollections(
        numHosts, numIterations, includeIdleMeasurements);
}

// Generate interesting values.
const equivalentStrings = ['a', 'A', 'b', 'B'];
const equivalentNumbers = [7, NumberInt(7), NumberLong(7), NumberDecimal(7)];
function generateInterestingValues() {
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
    values = values.concat(equivalentStrings).concat(equivalentNumbers);

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

function getMapInterestingValuesToEquivalentsStage() {
    const firstElemInId = {$arrayElemAt: ["$_id", 0]};
    const isIdArray = {$isArray: "$_id"};
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

function createInterestingCollections() {
    const collation = {locale: 'en_US', strength: 2};

    // Prepare timeseries collection.
    const tsCollName = "in";
    assert.commandWorked(testDB.createCollection(
        tsCollName, {timeseries: {timeField: "time", metaField: "tags"}, collation}));
    const tsColl = testDB[tsCollName];

    const interestingValues = generateInterestingValues();
    assert.commandWorked(tsColl.insertMany(interestingValues));

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
    assert.commandWorked(observerColl.insertMany(tsColl.find().toArray()));

    return [tsColl, observerColl];
}

function verifyTsResults({pipeline, precedingFilter, expectStage, prepareTest}) {
    const [tsColl, observerColl] = prepareTest();
    verifyLastpoint({tsColl, observerColl, pipeline, precedingFilter, expectStage});

    // Drop collections after test.
    tsColl.drop();
    observerColl.drop();
}

function verifyTsResultsWithAndWithoutIndex({
    pipeline,
    index,
    bucketsIndex,
    precedingFilter,
    expectStage,
    prepareCollections,
    noSortInCursor
}) {
    // Test without indexes first.
    const prepareTest = prepareCollections;
    const expectNoIndexStage = ({explain, precedingFilter}) =>
        expectCollScan({explain, precedingFilter, noSortInCursor});
    verifyTsResults({pipeline, precedingFilter, expectStage: expectNoIndexStage, prepareTest});

    // Test with indexes.
    verifyTsResults({
        pipeline,
        precedingFilter,
        expectStage,
        prepareTest: () => {
            const [tsColl, observerColl] = prepareCollections();

            // Create index on the timeseries collection.
            tsColl.createIndex(index);

            // Create an additional secondary index directly on the buckets collection so that we
            // can test the DISTINCT_SCAN optimization when time is sorted in ascending order.
            if (bucketsIndex) {
                const bucketsColl = testDB["system.buckets.in"];
                bucketsColl.createIndex(bucketsIndex);
            }

            return [tsColl, observerColl];
        }
    });
}

function expectDistinctScan({explain}) {
    // The query can utilize DISTINCT_SCAN.
    assert.neq(getAggPlanStage(explain, "DISTINCT_SCAN"), null, explain);

    // Pipelines that use the DISTINCT_SCAN optimization should not also have a blocking sort.
    assert.eq(getAggPlanStage(explain, "SORT"), null, explain);
}

function expectCollScan({explain, precedingFilter, noSortInCursor}) {
    if (noSortInCursor) {
        // We need a separate sort stage.
        assert.eq(getAggPlanStage(explain, "SORT"), null, explain);
    } else {
        // $sort can be pushed into the cursor layer.
        assert.neq(getAggPlanStage(explain, "SORT"), null, explain);
    }

    // At the bottom, there should be a COLLSCAN.
    const collScanStage = getAggPlanStage(explain, "COLLSCAN");
    assert.neq(collScanStage, null, explain);
    if (precedingFilter) {
        assert.eq(precedingFilter, collScanStage.filter, collScanStage);
    }
}

function expectIxscan({explain, noSortInCursor}) {
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

function getGroupStage(accumulator, extraFields = []) {
    let innerGroup = {_id: "$tags.hostid"};
    for (const f of extraFields.concat(["usage_user", "usage_guest", "usage_idle"])) {
        innerGroup[f] = {[accumulator]: "$" + f};
    }
    return {$group: innerGroup};
}

/**
    Test cases:
     1. Lastpoint queries on indexes with descending time and $first (DISTINCT_SCAN).
     2. Lastpoint queries on indexes with ascending time and $last (no DISTINCT_SCAN).
     3. Lastpoint queries on indexes with ascending time and $last and an additional secondary
    index so that we can use the DISTINCT_SCAN optimization.
*/
const testCases = [
    {time: -1, useBucketsIndex: false},
    {time: 1, useBucketsIndex: false},
    {time: 1, useBucketsIndex: true}
];

for (const {time, useBucketsIndex} of testCases) {
    const isTimeDescending = time < 0;
    const canUseDistinct = isTimeDescending || useBucketsIndex;
    const accumulator = isTimeDescending ? "$first" : "$last";
    const groupStage = getGroupStage(accumulator);

    // Test both directions of the metaField sort for each direction of time.
    for (const metaDir of [1, -1]) {
        const index = {"tags.hostid": metaDir, time};
        const bucketsIndex = useBucketsIndex
            ? {"meta.hostid": metaDir, "control.max.time": 1, "control.min.time": 1}
            : undefined;
        const canSortOnTimeUseDistinct = (metaDir > 0) && (isTimeDescending || useBucketsIndex);

        // Test pipeline without a preceding $match stage with sort only on time.
        verifyTsResultsWithAndWithoutIndex({
            pipeline: [{$sort: {time}}, groupStage],
            index,
            bucketsIndex,
            expectStage: (canSortOnTimeUseDistinct ? expectDistinctScan : expectCollScan),
            prepareCollections: createBoringCollections,
        });

        // Test pipeline without a preceding $match stage with a sort on the index.
        verifyTsResultsWithAndWithoutIndex({
            pipeline: [{$sort: index}, groupStage],
            index,
            bucketsIndex,
            expectStage: (canUseDistinct ? expectDistinctScan : expectCollScan),
            prepareCollections: createBoringCollections,
        });

        // Test pipeline without a projection to ensure that we correctly evaluate
        // computedMetaProjFields in the rewrite. Note that we can't get a DISTINCT_SCAN here due to
        // the projection.
        {
            const expectStage = ({explain}) => expectCollScan({explain, noSortInCursor: true});
            verifyTsResultsWithAndWithoutIndex({
                pipeline: [
                    {$set: {abc: {$add: [1, "$tags.hostid"]}}},
                    {$sort: index},
                    getGroupStage(accumulator, ["abc"]),
                ],
                index,
                bucketsIndex,
                noSortInCursor: true,
                expectStage,
                prepareCollections: createBoringCollections,
            });
        }

        // Test pipeline without a preceding $match stage which has an extra idle measurement. This
        // verifies that the query rewrite correctly returns missing fields.
        verifyTsResultsWithAndWithoutIndex({
            pipeline: [{$sort: index}, groupStage],
            index,
            bucketsIndex,
            expectStage: (canUseDistinct ? expectDistinctScan : expectCollScan),
            prepareCollections: () => createBoringCollections(true /* includeIdleMeasurements */),
        });

        // Test interesting metaField values.
        {
            const expectIxscanNoSort = ({explain}) => expectIxscan({explain, noSortInCursor: true});

            // Verifies that the '_id' of each group matches one of the equivalent '_id' values.
            const mapToEquivalentIdStage = getMapInterestingValuesToEquivalentsStage();

            // Test pipeline with sort only on time and interesting metaField values.
            verifyTsResultsWithAndWithoutIndex({
                pipeline: [{$sort: {time}}, groupStage, mapToEquivalentIdStage],
                index,
                bucketsIndex,
                // We get an index scan here because the index on interesting values is multikey.
                expectStage: (canSortOnTimeUseDistinct ? expectIxscanNoSort : expectCollScan),
                prepareCollections: createInterestingCollections,
            });

            // Test pipeline without a preceding $match stage and interesting metaField values.
            verifyTsResultsWithAndWithoutIndex({
                pipeline: [{$sort: index}, groupStage, mapToEquivalentIdStage],
                index,
                bucketsIndex,
                // We get an index scan here because the index on interesting values is multikey, so
                // we cannot have a DISTINCT_SCAN.
                expectStage: (canUseDistinct ? expectIxscanNoSort : expectCollScan),
                prepareCollections: createInterestingCollections,
            });
        }

        // Test pipeline with a preceding $match stage.
        function testWithMatch(matchStage, precedingFilter) {
            verifyTsResultsWithAndWithoutIndex({
                pipeline: [matchStage, {$sort: index}, groupStage],
                index,
                bucketsIndex,
                precedingFilter,
                expectStage: canUseDistinct ? expectDistinctScan : expectIxscan,
                prepareCollections: createBoringCollections,
            });
        }

        // Test pipeline with an equality $match stage.
        testWithMatch({$match: {"tags.hostid": 0}}, {"meta.hostid": {$eq: 0}});

        // Test pipeline with an inequality $match stage.
        testWithMatch({$match: {"tags.hostid": {$ne: 0}}}, {"meta.hostid": {$not: {$eq: 0}}});

        // Test pipeline with a $match stage that uses a $gt query.
        testWithMatch({$match: {"tags.hostid": {$gt: 5}}}, {"meta.hostid": {$gt: 5}});

        // Test pipeline with a $match stage that uses a $lt query.
        testWithMatch({$match: {"tags.hostid": {$lt: 5}}}, {"meta.hostid": {$lt: 5}});
    }
}
})();
