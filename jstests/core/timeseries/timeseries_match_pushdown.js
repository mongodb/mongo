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
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";

const coll = db.timeseries_match_pushdown;
const timeField = 'time';
const metaField = 'meta';
const measureField = 'a';

// The docs and queries are designed so that some buckets match the query entirely, some buckets
// match the query partially, and some with no matches.
const defaultDocs = [
    {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 0},
    {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 1},
    {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
    {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
    {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
    {[timeField]: ISODate('2022-01-01T00:00:06'), [metaField]: 2},
    {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: null, [metaField]: 2},
    {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: [1, 2, 3], [metaField]: 2}
];

/**
 * Setup the collection and run a $match query with the specified 'eventFilter' or a 'pipeline'.
 * Assert the 'wholeBucketFilter' is attached correctly to the unpacking stage, and has the expected
 * result 'expectedDocs'.
 */
const runTest = function({docsToInsert, pipeline, eventFilter, wholeBucketFilter, expectedDocs}) {
    // Set up the collection. Each test will have it's own collection setup.
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField, metaField}}));
    // Insert documents into the collection.
    if (!docsToInsert) {
        docsToInsert = defaultDocs;
    }
    assert.commandWorked(coll.insert(docsToInsert));

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

const aTime = ISODate('2022-01-01T00:00:03');
const bTime = ISODate('2022-01-01T00:00:07');
const bMeta = 3;
const aMeasure = 3;
const minTimeField = `control.min.${timeField}`;
const maxTimeField = `control.max.${timeField}`;

// $gt on time
runTest({
    eventFilter: {[timeField]: {$gt: aTime}},
    wholeBucketFilter: {[minTimeField]: {$gt: aTime}},
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: null, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: [1, 2, 3], [metaField]: 2},
    ],
});

// $gt on measurement
runTest({
    eventFilter: {[measureField]: {$gt: aMeasure}},
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
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
        {[timeField]: ISODate('2022-01-01T00:00:06'), [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: null, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: [1, 2, 3], [metaField]: 2},
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
        {[timeField]: ISODate('2022-01-01T00:00:06'), [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: null, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: [1, 2, 3], [metaField]: 2}
    ],
});

// $gte on measurement
runTest({
    eventFilter: {[measureField]: {$gte: aMeasure}},
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: [1, 2, 3], [metaField]: 2}
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
        {[timeField]: ISODate('2022-01-01T00:00:06'), [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: null, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: [1, 2, 3], [metaField]: 2}
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
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: [1, 2, 3], [metaField]: 2}
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
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: [1, 2, 3], [metaField]: 2}
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
    docsToInsert: [
        ...defaultDocs,
        {[timeField]: ISODate('2022-01-01T00:00:06'), [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: null, [metaField]: 1}
    ],
    eventFilter: {[measureField]: {$eq: aMeasure}},
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: [1, 2, 3], [metaField]: 2}
    ],
});

// $and on time
runTest({
    docsToInsert: [
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: 7, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: 8, [metaField]: 3},
    ],
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

//$or on time
runTest({
    docsToInsert: [
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 0},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: 7, [metaField]: 3},
    ],
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
    ],
});

// $match on time and meta
runTest({
    docsToInsert: [
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 0},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:07'), [measureField]: 7, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:08'), [measureField]: 8, [metaField]: 3},
    ],
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

// $match on time and meta inside $expr. There should not be a wholeBucketFilter, since the entire
// $and expression cannot be rewritten as a MatchExpression, and for $expr predicates we only
// generate a wholeBucketFilter for single predicates on the timeField.
runTest({
    docsToInsert: [
        {[timeField]: ISODate('2022-01-01T00:00:03'), [metaField]: 1, [measureField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [metaField]: 1, [measureField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [metaField]: 2, [measureField]: 3},
    ],
    pipeline: [{
        $match: {
            $expr: {
                $and:
                    [{$eq: [`$${metaField}`, `$${measureField}`]}, {$gt: [`$${timeField}`, aTime]}]
            }
        }
    }],
    eventFilter: {
        $and: [
            {[timeField]: {$_internalExprGt: aTime}},
            {
                $expr: {
                    $and: [
                        {$eq: [`$${metaField}`, `$${measureField}`]},
                        {$gt: [`$${timeField}`, {$const: aTime}]}
                    ]
                }
            },
        ]
    },
    expectedDocs: [{[timeField]: ISODate('2022-01-01T00:00:04'), [metaField]: 1, [measureField]: 1}]
});

// $match on time and meta inside $expr. The entire $and expression can be rewritten into a
// MatchExpression. However, for $expr predicates we only generate a wholeBucketFilter for single
// predicates on the timeField.
runTest({
    docsToInsert: [
        {[timeField]: ISODate('2022-01-01T00:00:03'), [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [metaField]: 2},
    ],
    pipeline:
        [{$match: {$expr: {$and: [{$eq: [`$${metaField}`, 1]}, {$gt: [`$${timeField}`, aTime]}]}}}],
    eventFilter: {
        $and: [
            {[timeField]: {$_internalExprGt: aTime}},
            {
                $expr: {
                    $and: [
                        {$eq: [`$${metaField}`, {$const: 1}]},
                        {$gt: [`$${timeField}`, {$const: aTime}]}
                    ]
                }
            },
            {[metaField]: {$_internalExprEq: 1}},
            {[timeField]: {$_internalExprGt: aTime}},
        ]
    },
    expectedDocs: [{[timeField]: ISODate('2022-01-01T00:00:04'), [metaField]: 1}]
});

// $match on time or meta
runTest({
    docsToInsert: [
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 0},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 3},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 4},
    ],
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
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 4},
    ],
});

// double $match
runTest({
    docsToInsert: [
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 0},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 3},
    ],
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
    docsToInsert: [
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 0},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [measureField]: 4, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 3},
    ],
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

// $and inside $expr with comparison on meta and measurement. There should not be a
// wholeBucketFilter, since the entire $and expression cannot be rewritten as a MatchExpression, and
// for $expr predicates we only generate a wholeBucketFilter for single predicates on the timeField.
runTest({
    docsToInsert: [
        {[timeField]: ISODate('2022-01-01T00:00:00'), [measureField]: 0, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 3},
    ],
    pipeline: [{
        $match: {
            $expr:
                {$and: [{$lt: [`$${measureField}`, `$${metaField}`]}, {$gt: [`$${metaField}`, 1]}]}
        }
    }],
    eventFilter: {
        $and: [
            {"meta": {$_internalExprGt: 1}},
            {
                $expr: {
                    $and: [
                        {$lt: [`$${measureField}`, `$${metaField}`]},
                        {$gt: [`$${metaField}`, {$const: 1}]}
                    ]
                }
            }
        ]
    },
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [metaField]: 2},
    ]
});

// Same test as above, but the entire $and expression can be rewritten as a MatchExpression.
// However, for $expr predicates we only generate a wholeBucketFilter for single predicates on the
// timeField.
runTest({
    docsToInsert: [
        {[timeField]: ISODate('2022-01-01T00:00:00'), [measureField]: 0, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: null, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:06'), [measureField]: 6, [metaField]: 3},
    ],
    pipeline:
        [{$match: {$expr: {$and: [{$gte: [`$${measureField}`, 2]}, {$lt: [`$${metaField}`, 3]}]}}}],
    eventFilter: {
        $and: [
            {[measureField]: {$_internalExprGte: 2}},
            {
                $expr: {
                    $and: [
                        {$gte: [`$${measureField}`, {$const: 2}]},
                        {$lt: [`$${metaField}`, {$const: 3}]}
                    ]
                }
            },
            {[measureField]: {$_internalExprGte: 2}},
            {[metaField]: {$_internalExprLt: 3}},
        ]
    },
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 2},
    ]
});

// Same test as above but with $or and different comparison operators.
runTest({
    docsToInsert: [
        {[timeField]: ISODate('2022-01-01T00:00:00'), [measureField]: 0, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:05'), [measureField]: 5, [metaField]: 3},
    ],
    pipeline: [{
        $match: {
            $expr:
                {$or: [{$lt: [`$${measureField}`, `$${metaField}`]}, {$lte: [`$${metaField}`, 2]}]}
        }
    }],
    eventFilter: {
        $expr: {
            $or: [
                {$lt: [`$${measureField}`, `$${metaField}`]},
                {$lte: [`$${metaField}`, {$const: 2}]}
            ]
        }
    },
    expectedDocs: [
        {[timeField]: ISODate('2022-01-01T00:00:00'), [measureField]: 0, [metaField]: 1},
        {[timeField]: ISODate('2022-01-01T00:00:01'), [measureField]: 1, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:02'), [measureField]: 2, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:03'), [measureField]: 3, [metaField]: 2},
        {[timeField]: ISODate('2022-01-01T00:00:04'), [metaField]: 2},
    ]
});
