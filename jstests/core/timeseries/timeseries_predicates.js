/**
 * Test the input/output behavior of some predicates on time-series collections.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_fcv_52,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const coll = db.timeseries_predicates_normal;
const tsColl = db.timeseries_predicates_timeseries;
coll.drop();
tsColl.drop();
assert.commandWorked(
    db.createCollection(tsColl.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));
const bucketsColl = db.getCollection('system.buckets.' + tsColl.getName());

// Test that 'predicate' behaves correctly on the example documents,
// by comparing the result on a time-series collection against a normal collection.
function checkPredicateResult(predicate, documents) {
    assert.commandWorked(coll.deleteMany({}));
    assert.commandWorked(bucketsColl.deleteMany({}));
    assert.commandWorked(coll.insert(documents));
    assert.commandWorked(tsColl.insert(documents));

    const normalResult = coll.aggregate({$match: predicate}).toArray();
    const tsResult = tsColl.aggregate({$match: predicate}).toArray();
    assert.sameMembers(normalResult, tsResult);
}

// Test that 'predicate' behaves correctly, no matter how the documents are bucketed:
// insert the documents with different combinations of metadata to change how they are bucketed.
// 'documents' should be small, since this runs 2^N tests.
function checkAllBucketings(predicate, documents) {
    for (const doc of documents) {
        doc._id = ObjectId();
        doc.time = ISODate();
    }

    // For N documents, there are 2^N ways to assign them to buckets A and B.
    const numDocs = documents.length;
    const numBucketings = 1 << numDocs;
    for (let bucketing = 0; bucketing < numBucketings; ++bucketing) {
        // The ith bit tells you how to assign documents[i].
        const labeledDocs = documents.map(
            (doc, i) => Object.merge(doc, {meta: {bucket: bucketing & (1 << i)}}, true /*deep*/));
        checkPredicateResult(predicate, labeledDocs);
    }
}

// Test $in...
{
    // ... with a simple list of equalities.
    checkAllBucketings({x: {$in: [2, 3, 4]}}, [
        {x: 1},
        {x: 2},
        {x: 3},
        {x: 42},
    ]);

    // ... with equalities which contain an array.
    checkAllBucketings({x: {$in: [1, [2, 3, 4], 5]}}, [
        {},
        {x: 1},
        {x: 2},
        {x: [2, 3]},
        {x: [2, 3, 4]},
        {x: 5},
        {x: 42},
    ]);
}

// $exists: true
checkAllBucketings({x: {$exists: true}}, [
    {},
    {x: 2},
    {x: ISODate()},
    {x: null},
    {x: undefined},
]);

// Test $or...
{
    // ... on metric + meta.
    checkAllBucketings({
        $or: [
            {x: {$lt: 0}},
            {'meta.y': {$gt: 0}},
        ]
    },
                       [
                           {x: +1, meta: {y: -1}},
                           {x: +1, meta: {y: +1}},
                           {x: -1, meta: {y: -1}},
                           {x: -1, meta: {y: +1}},
                       ]);

    // ... when one argument can't be pushed down.
    checkAllBucketings({
        $or: [
            {x: {$lt: 0}},
            {y: {$exists: false}},
        ]
    },
                       [
                           {x: -1},
                           {x: +1},
                           {x: -1, y: 'asdf'},
                           {x: +1, y: 'asdf'},
                       ]);

    // ... when neither argument can be pushed down.
    checkAllBucketings({
        $or: [
            {x: {$exists: false}},
            {y: {$exists: false}},
        ]
    },
                       [
                           {},
                           {x: 'qwer'},
                           {y: 'asdf'},
                           {x: 'qwer', y: 'asdf'},
                       ]);
}

// Test $and...
{
    // ... on metric + meta.
    checkAllBucketings({
        $and: [
            {x: {$lt: 0}},
            {'meta.y': {$gt: 0}},
        ]
    },
                       [
                           {x: +1, meta: {y: -1}},
                           {x: +1, meta: {y: +1}},
                           {x: -1, meta: {y: -1}},
                           {x: -1, meta: {y: +1}},
                       ]);

    // ... when one argument can't be pushed down.
    checkAllBucketings({
        $and: [
            {x: {$lt: 0}},
            {y: {$exists: false}},
        ]
    },
                       [
                           {x: -1},
                           {x: +1},
                           {x: -1, y: 'asdf'},
                           {x: +1, y: 'asdf'},
                       ]);

    // ... when neither argument can be pushed down.
    checkAllBucketings({
        $and: [
            {x: {$exists: false}},
            {y: {$exists: false}},
        ]
    },
                       [
                           {},
                           {x: 'qwer'},
                           {y: 'asdf'},
                           {x: 'qwer', y: 'asdf'},
                       ]);
}

// Test nested $and / $or.
checkAllBucketings({
    // The top-level $or prevents us from splitting into 2 top-level $match stages.
    $or: [
        {
            $and: [
                {'meta.a': {$gt: 0}},
                {'x': {$lt: 0}},
            ]
        },
        {
            $and: [
                {'meta.b': {$gte: 0}},
                {time: {$gt: ISODate('2020-01-01')}},
            ]
        },
    ]
},
                   [
                       {meta: {a: -1, b: -1}, x: -1, time: ISODate('2020-02-01')},
                       {meta: {a: -1, b: -1}, x: -1, time: ISODate('2019-12-31')},
                       {meta: {a: -1, b: -1}, x: +1, time: ISODate('2020-02-01')},
                       {meta: {a: -1, b: -1}, x: +1, time: ISODate('2019-12-31')},

                       {meta: {a: +1, b: -1}, x: -1, time: ISODate('2020-02-01')},
                       {meta: {a: +1, b: -1}, x: -1, time: ISODate('2019-12-31')},
                       {meta: {a: +1, b: -1}, x: +1, time: ISODate('2020-02-01')},
                       {meta: {a: +1, b: -1}, x: +1, time: ISODate('2019-12-31')},
                   ]);

// Test nested $and / $or where some leaf predicates cannot be pushed down.
checkAllBucketings({
    $or: [
        {
            $and: [
                {'meta.a': {$gt: 0}},
                {'x': {$exists: false}},
            ]
        },
        {
            $and: [
                {'meta.b': {$gte: 0}},
                {time: {$gt: ISODate('2020-01-01')}},
            ]
        },
    ]
},
                   [
                       {meta: {a: -1, b: -1}, time: ISODate('2020-02-01')},
                       {meta: {a: -1, b: -1}, time: ISODate('2019-12-31')},
                       {meta: {a: -1, b: -1}, x: 'asdf', time: ISODate('2020-02-01')},
                       {meta: {a: -1, b: -1}, x: 'asdf', time: ISODate('2019-12-31')},

                       {meta: {a: +1, b: -1}, time: ISODate('2020-02-01')},
                       {meta: {a: +1, b: -1}, time: ISODate('2019-12-31')},
                       {meta: {a: +1, b: -1}, x: 'asdf', time: ISODate('2020-02-01')},
                       {meta: {a: +1, b: -1}, x: 'asdf', time: ISODate('2019-12-31')},
                   ]);
})();
