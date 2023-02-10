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
    db.createCollection(tsColl.getName(), {timeseries: {timeField: 'time', metaField: 'mt'}}));
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
        doc.time = doc.time || ISODate();
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
    // ... on metric + mt.
    checkAllBucketings({
        $or: [
            {x: {$lt: 0}},
            {'mt.y': {$gt: 0}},
        ]
    },
                       [
                           {x: +1, mt: {y: -1}},
                           {x: +1, mt: {y: +1}},
                           {x: -1, mt: {y: -1}},
                           {x: -1, mt: {y: +1}},
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
    // ... on metric + mt.
    checkAllBucketings({
        $and: [
            {x: {$lt: 0}},
            {'mt.y': {$gt: 0}},
        ]
    },
                       [
                           {x: +1, mt: {y: -1}},
                           {x: +1, mt: {y: +1}},
                           {x: -1, mt: {y: -1}},
                           {x: -1, mt: {y: +1}},
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
                {'mt.a': {$gt: 0}},
                {'x': {$lt: 0}},
            ]
        },
        {
            $and: [
                {'mt.b': {$gte: 0}},
                {time: {$gt: ISODate('2020-01-01')}},
            ]
        },
    ]
},
                   [
                       {mt: {a: -1, b: -1}, x: -1, time: ISODate('2020-02-01')},
                       {mt: {a: -1, b: -1}, x: -1, time: ISODate('2019-12-31')},
                       {mt: {a: -1, b: -1}, x: +1, time: ISODate('2020-02-01')},
                       {mt: {a: -1, b: -1}, x: +1, time: ISODate('2019-12-31')},

                       {mt: {a: +1, b: -1}, x: -1, time: ISODate('2020-02-01')},
                       {mt: {a: +1, b: -1}, x: -1, time: ISODate('2019-12-31')},
                       {mt: {a: +1, b: -1}, x: +1, time: ISODate('2020-02-01')},
                       {mt: {a: +1, b: -1}, x: +1, time: ISODate('2019-12-31')},
                   ]);

// Test nested $and / $or where some leaf predicates cannot be pushed down.
checkAllBucketings({
    $or: [
        {
            $and: [
                {'mt.a': {$gt: 0}},
                {'x': {$exists: false}},
            ]
        },
        {
            $and: [
                {'mt.b': {$gte: 0}},
                {time: {$gt: ISODate('2020-01-01')}},
            ]
        },
    ]
},
                   [
                       {mt: {a: -1, b: -1}, time: ISODate('2020-02-01')},
                       {mt: {a: -1, b: -1}, time: ISODate('2019-12-31')},
                       {mt: {a: -1, b: -1}, x: 'asdf', time: ISODate('2020-02-01')},
                       {mt: {a: -1, b: -1}, x: 'asdf', time: ISODate('2019-12-31')},

                       {mt: {a: +1, b: -1}, time: ISODate('2020-02-01')},
                       {mt: {a: +1, b: -1}, time: ISODate('2019-12-31')},
                       {mt: {a: +1, b: -1}, x: 'asdf', time: ISODate('2020-02-01')},
                       {mt: {a: +1, b: -1}, x: 'asdf', time: ISODate('2019-12-31')},
                   ]);

// Test $exists on mt, inside $or.
checkAllBucketings({
    $or: [
        {"mt.a": {$exists: true}},
        {"x": {$gt: 2}},
    ]
},
                   [
                       {mt: {a: 1}, x: 1},
                       {mt: {a: 2}, x: 2},
                       {mt: {a: 3}, x: 3},
                       {mt: {a: 4}, x: 4},
                       {mt: {}, x: 1},
                       {mt: {}, x: 2},
                       {mt: {}, x: 3},
                       {mt: {}, x: 4},
                   ]);

// Test $in on mt, inside $or.
checkAllBucketings({
    $or: [
        {"mt.a": {$in: [1, 3]}},
        {"x": {$gt: 2}},
    ]
},
                   [
                       {mt: {a: 1}, x: 1},
                       {mt: {a: 2}, x: 2},
                       {mt: {a: 3}, x: 3},
                       {mt: {a: 4}, x: 4},
                       {mt: {}, x: 1},
                       {mt: {}, x: 2},
                       {mt: {}, x: 3},
                       {mt: {}, x: 4},
                   ]);

// Test geo predicates on mt, inside $or.
for (const pred of ['$geoWithin', '$geoIntersects']) {
    checkAllBucketings({
        $or: [
            {
                "mt.location": {
                    [pred]: {
                        $geometry: {
                            type: "Polygon",
                            coordinates: [[
                                [0, 0],
                                [0, 3],
                                [3, 3],
                                [3, 0],
                                [0, 0],
                            ]]
                        }
                    }
                }
            },
            {x: {$gt: 2}},
        ]
    },
                       [
                           {mt: {location: [1, 1]}, x: 1},
                           {mt: {location: [1, 1]}, x: 2},
                           {mt: {location: [1, 1]}, x: 3},
                           {mt: {location: [1, 1]}, x: 4},
                           {mt: {location: [5, 5]}, x: 1},
                           {mt: {location: [5, 5]}, x: 2},
                           {mt: {location: [5, 5]}, x: 3},
                           {mt: {location: [5, 5]}, x: 4},
                       ]);
}

// Test $mod on mt, inside $or.
// $mod is an example of a predicate that we don't handle specially in time-series optimizations:
// it can be pushed down if and only if it's on a metadata field.
checkAllBucketings({
    $or: [
        {"mt.a": {$mod: [2, 0]}},
        {"x": {$gt: 4}},
    ]
},
                   [
                       {mt: {a: 1}, x: 1},
                       {mt: {a: 2}, x: 2},
                       {mt: {a: 3}, x: 3},
                       {mt: {a: 4}, x: 4},
                       {mt: {a: 5}, x: 5},
                       {mt: {a: 6}, x: 6},
                       {mt: {a: 7}, x: 7},
                       {mt: {a: 8}, x: 8},
                   ]);

// Test $elemMatch on mt, inside $or.
checkAllBucketings({
    $or: [
        {"mt.a": {$elemMatch: {b: 3}}},
        {"x": {$gt: 4}},
    ]
},
                   [
                       {x: 1, mt: {a: []}},
                       {x: 2, mt: {a: [{b: 2}]}},
                       {x: 3, mt: {a: [{b: 3}]}},
                       {x: 4, mt: {a: [{b: 2}, {b: 3}]}},
                       {x: 5, mt: {a: []}},
                       {x: 6, mt: {a: [{b: 2}]}},
                       {x: 7, mt: {a: [{b: 3}]}},
                       {x: 8, mt: {a: [{b: 2}, {b: 3}]}},
                   ]);
checkAllBucketings({
    $or: [
        {"mt.a": {$elemMatch: {b: 2, c: 3}}},
        {"x": {$gt: 3}},
    ]
},
                   [
                       {x: 1, mt: {a: []}},
                       {x: 2, mt: {a: [{b: 2, c: 3}]}},
                       {x: 3, mt: {a: [{b: 2}, {c: 3}]}},
                       {x: 4, mt: {a: []}},
                       {x: 5, mt: {a: [{b: 2, c: 3}]}},
                       {x: 6, mt: {a: [{b: 2}, {c: 3}]}},
                   ]);

// Test a standalone $elemMatch on mt.
checkAllBucketings({"mt.a": {$elemMatch: {b: 3}}}, [
    {mt: {a: []}},
    {mt: {a: [{b: 2}]}},
    {mt: {a: [{b: 3}]}},
    {mt: {a: [{b: 2}, {b: 3}]}},
    {mt: {a: []}},
    {mt: {a: [{b: 2}]}},
    {mt: {a: [{b: 3}]}},
    {mt: {a: [{b: 2}, {b: 3}]}},
]);

// Test a standalone $size on mt.
checkAllBucketings({"mt.a": {$size: 1}}, [
    {mt: {a: []}},
    {mt: {a: [{b: 2}]}},
    {mt: {a: [{b: 3}]}},
    {mt: {a: [{b: 2}, {b: 3}]}},
    {mt: {a: []}},
    {mt: {a: [{b: 2}]}},
    {mt: {a: [{b: 3}]}},
    {mt: {a: [{b: 2}, {b: 3}]}},
]);
})();
