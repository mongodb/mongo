/**
 * Test that $lookup behaves correctly for null values, as well as "missing" and undefined.
 *
 * @tags: [
 *   # We don't yet support lookup into a sharded collection.
 *   assumes_unsharded_collection,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");

const localColl = db.lookup_null_semantics_local;
const foreignColl = db.lookup_null_semantics_foreign;

localColl.drop();
foreignColl.drop();

assert.commandWorked(localColl.insert([
    {_id: 0},
    {_id: 1, a: null},
    {_id: 2, a: 9},
    {_id: 3, a: [null, 9]},
]));
assert.commandWorked(foreignColl.insert([
    {_id: 0},
    {_id: 1, b: null},
    {_id: 2, b: undefined},
    {_id: 3, b: 9},
    {_id: 4, b: [null, 9]},
    {_id: 5, b: 42},
    {_id: 6, b: [undefined, 9]},
    {_id: 7, b: [9, 10]},
]));

const actualResults = localColl.aggregate([{
    $lookup: {from: foreignColl.getName(), localField: "a", foreignField: "b", as: "lookupResults"}
}]).toArray();

const expectedResults = [
    // Missing on the left-hand side results in a lookup with $eq:null semantics on the left-hand
    // side. Namely, we expect this document to join with null, missing, and undefined, or arrays
    // thereof.
    {
        _id: 0,
        lookupResults: [
            {_id: 0},
            {_id: 1, b: null},
            {_id: 2, b: undefined},
            {_id: 4, b: [null, 9]},
            {_id: 6, b: [undefined, 9]}
        ]
    },
    // Null on the left-hand side is the same as the missing case above.
    {
        _id: 1,
        a: null,
        lookupResults: [
            {_id: 0},
            {_id: 1, b: null},
            {_id: 2, b: undefined},
            {_id: 4, b: [null, 9]},
            {_id: 6, b: [undefined, 9]}
        ]
    },
    // A "negative" test-case where the value being looked up is not nullish.
    {
        _id: 2,
        a: 9,
        lookupResults: [
            {_id: 3, b: 9},
            {_id: 4, b: [null, 9]},
            {_id: 6, b: [undefined, 9]},
            {_id: 7, b: [9, 10]},
        ]
    },
    // Here we are looking up both null and a scalar. We expected missing, null, and undefined to
    // match in addition to the matches due to the scalar.
    {
        _id: 3,
        a: [null, 9],
        lookupResults: [
            {_id: 0},
            {_id: 1, b: null},
            {_id: 2, b: undefined},
            {_id: 3, b: 9},
            {_id: 4, b: [null, 9]},
            {_id: 6, b: [undefined, 9]},
            {_id: 7, b: [9, 10]},
        ]
    },
];

assertArrayEq({actual: actualResults, expected: expectedResults});

// The results should not change there is an index available on the right-hand side.
assert.commandWorked(foreignColl.createIndex({b: 1}));
assertArrayEq({actual: actualResults, expected: expectedResults});

// If the left-hand side collection has a value of undefined for "localField", then the query will
// fail. This is a consequence of the fact that queries which explicitly compare to undefined, such
// as {$eq:undefined}, are banned. Arguably this behavior could be improved, but we are unlikely to
// change it given that the undefined BSON type has been deprecated for many years.
assert.commandWorked(localColl.insert({a: undefined}));
assert.throws(() => {
    localColl.aggregate([{
    $lookup: {from: foreignColl.getName(), localField: "a", foreignField: "b", as: "lookupResults"}
}]).toArray();
});
}());
