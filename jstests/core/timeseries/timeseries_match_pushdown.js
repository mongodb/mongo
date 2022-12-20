/**
 * Tests that the $match stage followed by unpacking stage has been pushed down with correct
 * predicates.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_fcv_62,
 *   does_not_support_stepdowns,
 *   directly_against_shardsvrs_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getAggPlanStages

const coll = db.timeseries_match_pushdown;
coll.drop();

const timeField = 'time';
const metaField = 'meta';
const measureField = 'a';
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField, metaField}}));

// Insert documents into the collection. The bucketing is designed so that some buckets match the
// query entirely, some buckets match the query partially, and some with no matches.
assert.commandWorked(coll.insert([
    {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 0},
    {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 1},
    {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
    {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
    {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
    {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 3},
    {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: 7, [metaField]: 3},
    {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: 8, [metaField]: 3},
    {[timeField]: ISODate('2022-01-01T00:00:09'), [measureField]: 9, [metaField]: 4},
]));
const aTime = ISODate('2022-01-01T00:00:03');
const bTime = ISODate('2022-01-01T00:00:07');
const bMeta = 3;
const aMeasure = 3;

/**
 * Runs a $match query with the specified 'eventFilter' or a 'pipeline'.
 * Assert the 'wholeBucketFilter' is attached correctly to the unpacking stage, and has the expected
 * result 'expectedDocs'.
 */
const runTest = function({pipeline, eventFilter, wholeBucketFilter, expectedDocs}) {
    if (!pipeline) {
        pipeline = [{$match: eventFilter}];
    }
    const explain = assert.commandWorked(coll.explain().aggregate(pipeline));
    const unpackStages = getAggPlanStages(explain, '$_internalUnpackBucket');
    assert.eq(1,
              unpackStages.length,
              "Should only have a single $_internalUnpackBucket stage: " + tojson(explain));
    const unpackStage = unpackStages[0].$_internalUnpackBucket;
    assert.docEq(eventFilter, unpackStage.eventFilter, "Incorrect eventFilter: " + tojson(explain));
    if (wholeBucketFilter) {
        assert.docEq(wholeBucketFilter,
                     unpackStage.wholeBucketFilter,
                     "Incorrect wholeBucketFilter: " + tojson(explain));
    } else {
        assert(!unpackStage.wholeBucketFilter, "Incorrect wholeBucketFilter: " + tojson(explain));
    }

    const docs = coll.aggregate([...pipeline, {$sort: {time: 1}}]).toArray();
    assert.eq(docs.length, expectedDocs.length, "Incorrect docs: " + tojson(docs));
    docs.forEach((doc, i) => {
        // Do not need to check document _id, since checking time is already unique.
        delete doc._id;
        assert.docEq(expectedDocs[i], doc, "Incorrect docs: " + tojson(docs));
    });
};

const minTimeField = `control.min.${timeField}`;
const maxTimeField = `control.max.${timeField}`;

// $gt on time
runTest({
    eventFilter: {[timeField]: {$gt: aTime}},
    wholeBucketFilter: {[minTimeField]: {$gt: aTime}},
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: 7, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: 8, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:09'), [measureField]: 9, [metaField]: 4},
    ],
});

// $gt on measurement
runTest({
    eventFilter: {[measureField]: {$gt: aMeasure}},
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: 7, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: 8, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:09'), [measureField]: 9, [metaField]: 4},
    ],
});

// $gt in $expr on time
runTest({
    pipeline: [{$match: {$expr: {$gt: [`$${timeField}`, {$const: aTime}]}}}],
    eventFilter: {
        $and: [
            {[timeField]: {$_internalExprGt: aTime}},
            {$expr: {$gt: [`$${timeField}`, {$const: aTime}]}},
        ]
    },
    wholeBucketFilter: {
        $and: [
            {[minTimeField]: {$_internalExprGt: aTime}},
            {[minTimeField]: {$_internalExprGt: aTime}},
        ]
    },
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: 7, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: 8, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:09'), [measureField]: 9, [metaField]: 4},
    ],
});

// $gte on time
runTest({
    eventFilter: {[timeField]: {$gte: aTime}},
    wholeBucketFilter: {[minTimeField]: {$gte: aTime}},
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: 7, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: 8, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:09'), [measureField]: 9, [metaField]: 4},
    ],
});

// $gte on measurement
runTest({
    eventFilter: {[measureField]: {$gte: aMeasure}},
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: 7, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: 8, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:09'), [measureField]: 9, [metaField]: 4},
    ],
});

// $gte in $expr on time
runTest({
    pipeline: [{$match: {$expr: {$gte: [`$${timeField}`, {$const: aTime}]}}}],
    eventFilter: {
        $and: [
            {[timeField]: {$_internalExprGte: aTime}},
            {$expr: {$gte: [`$${timeField}`, {$const: aTime}]}},
        ]
    },
    wholeBucketFilter: {
        $and: [
            {[minTimeField]: {$_internalExprGte: aTime}},
            {[minTimeField]: {$_internalExprGte: aTime}},
        ]
    },
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: 7, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: 8, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:09'), [measureField]: 9, [metaField]: 4},
    ],
});

// $lt on time
runTest({
    eventFilter: {[timeField]: {$lt: aTime}},
    wholeBucketFilter: {[maxTimeField]: {$lt: aTime}},
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 0},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 1},
    ],
});

// $lt on measurement
runTest({
    eventFilter: {[measureField]: {$lt: aMeasure}},
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 0},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 1},
    ],
});

// $lt in $expr on time
runTest({
    pipeline: [{$match: {$expr: {$lt: [`$${timeField}`, {$const: aTime}]}}}],
    eventFilter: {
        $and: [
            {[timeField]: {$_internalExprLt: aTime}},
            {$expr: {$lt: [`$${timeField}`, {$const: aTime}]}},
        ]
    },
    wholeBucketFilter: {
        $and: [
            {[maxTimeField]: {$_internalExprLt: aTime}},
            {[maxTimeField]: {$_internalExprLt: aTime}},
        ]
    },
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 0},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 1},
    ],
});

// $lte on time
runTest({
    eventFilter: {[timeField]: {$lte: aTime}},
    wholeBucketFilter: {[maxTimeField]: {$lte: aTime}},
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 0},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
    ],
});

// $lte in $expr on time
runTest({
    pipeline: [{$match: {$expr: {$lte: [`$${timeField}`, {$const: aTime}]}}}],
    eventFilter: {
        $and: [
            {[timeField]: {$_internalExprLte: aTime}},
            {$expr: {$lte: [`$${timeField}`, {$const: aTime}]}},
        ]
    },
    wholeBucketFilter: {
        $and: [
            {[maxTimeField]: {$_internalExprLte: aTime}},
            {[maxTimeField]: {$_internalExprLte: aTime}},
        ]
    },
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 0},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
    ],
});

// $lte on measurement
runTest({
    eventFilter: {[measureField]: {$lte: aMeasure}},
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 0},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
    ],
});

// $eq on time
runTest({
    eventFilter: {[timeField]: {$eq: aTime}},
    wholeBucketFilter: {$and: [{[minTimeField]: {$eq: aTime}}, {[maxTimeField]: {$eq: aTime}}]},
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
    ],
});

// $eq in $expr on time
runTest({
    pipeline: [{$match: {$expr: {$eq: [`$${timeField}`, {$const: aTime}]}}}],
    eventFilter: {
        $and: [
            {[timeField]: {$_internalExprEq: aTime}},
            {$expr: {$eq: [`$${timeField}`, {$const: aTime}]}},
        ]
    },
    wholeBucketFilter: {
        $and: [
            {[minTimeField]: {$_internalExprEq: aTime}},
            {[maxTimeField]: {$_internalExprEq: aTime}},
            {[minTimeField]: {$_internalExprEq: aTime}},
            {[maxTimeField]: {$_internalExprEq: aTime}},
        ]
    },
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
    ],
});

// $eq on measurement
runTest({
    eventFilter: {[measureField]: {$eq: aMeasure}},
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
    ],
});

// $and on time
runTest({
    eventFilter: {$and: [{[timeField]: {$gt: aTime}}, {[timeField]: {$lt: bTime}}]},
    wholeBucketFilter: {
        $and: [
            {[minTimeField]: {$gt: aTime}},
            {[maxTimeField]: {$lt: bTime}},
        ]
    },
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 3},
    ],
});

// $or on time
runTest({
    eventFilter: {$or: [{[timeField]: {$lte: aTime}}, {[timeField]: {$gte: bTime}}]},
    wholeBucketFilter: {
        $or: [
            {[maxTimeField]: {$lte: aTime}},
            {[minTimeField]: {$gte: bTime}},
        ]
    },
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 0},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: 7, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: 8, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:09'), [measureField]: 9, [metaField]: 4},
    ],
});

// $match on time and meta
runTest({
    pipeline: [{$match: {$and: [{[timeField]: {$gt: aTime}}, {[metaField]: {$lte: bMeta}}]}}],
    eventFilter: {[timeField]: {$gt: aTime}},
    wholeBucketFilter: {
        [minTimeField]: {$gt: aTime},
    },
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: 7, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: 8, [metaField]: 3},
    ],
});

// $match on time or meta
runTest({
    eventFilter: {$or: [{[timeField]: {$lte: aTime}}, {[metaField]: {$gt: bMeta}}]},
    wholeBucketFilter: {
        $or: [
            {[maxTimeField]: {$lte: aTime}},
            {[metaField]: {$gt: bMeta}},
        ]
    },
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 0},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:09'), [measureField]: 9, [metaField]: 4},
    ],
});

// double $match
runTest({
    pipeline: [{$match: {[timeField]: {$gt: aTime}}}, {$match: {[timeField]: {$lt: bTime}}}],
    eventFilter: {$and: [{[timeField]: {$gt: aTime}}, {[timeField]: {$lt: bTime}}]},
    wholeBucketFilter: {
        $and: [
            {[minTimeField]: {$gt: aTime}},
            {[maxTimeField]: {$lt: bTime}},
        ]
    },
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 3},
    ],
});

// triple $match
runTest({
    pipeline: [
        {$match: {[timeField]: {$gt: aTime}}},
        {$match: {[timeField]: {$lt: bTime}}},
        {$match: {[timeField]: {$lt: aTime}}},
    ],
    eventFilter: {
        $and:
            [{[timeField]: {$gt: aTime}}, {[timeField]: {$lt: bTime}}, {[timeField]: {$lt: aTime}}]
    },
    wholeBucketFilter: {
        $and: [
            {[minTimeField]: {$gt: aTime}},
            {[maxTimeField]: {$lt: bTime}},
            {[maxTimeField]: {$lt: aTime}},
        ]
    },
    expectedDocs: [],
});
})();
