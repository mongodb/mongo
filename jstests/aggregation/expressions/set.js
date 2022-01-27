/**
 * Test the set expressions.
 */
(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");

const coll = db.expression_set;
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0, arr1: [1, 2, 3], arr2: [2, 3, 4]},
    {_id: 1, arr1: [1, 2, 3], arr2: [4, 5, 6]},
    {_id: 2, arr1: [1, 2, 3], arr2: []},
    {_id: 3, arr1: [], arr2: [4, 5, 6]},
    {_id: 4, arr1: [1, 2, 3], arr2: [2, 3]},
    {_id: 5, arr1: [2, 3], arr2: [2, 3, 4]},
    {_id: 6, arr1: [1, 2, 3], arr2: [1, 2, 3]},
    {_id: 7, arr1: [1, 2, 3], arr2: [1, 1, 2, 2, 3, 3]},
]));

const result = coll.aggregate([
                       {$sort: {_id: 1}},
                       {
                           $project: {
                               union: {$setUnion: ["$arr1", "$arr2"]},
                               intersection: {$setIntersection: ["$arr1", "$arr2"]},
                               difference: {$setDifference: ["$arr1", "$arr2"]},
                               isSubset: {$setIsSubset: ["$arr1", "$arr2"]},
                               equals: {$setEquals: ["$arr1", "$arr2"]},
                           }
                       }
                   ])
                   .toArray();

// The order of the output array elements is undefined for $setUnion, $setDifference and
// $setIntersection expressions. Hence we do a sort operation to get a consistent order.
const sortSetFields = document => Object.assign(document, {
    union: document.union.sort(),
    intersection: document.intersection.sort(),
    difference: document.difference.sort(),
});

assert.eq(result.map(sortSetFields), [
    {
        _id: 0,
        union: [1, 2, 3, 4],
        intersection: [2, 3],
        difference: [1],
        isSubset: false,
        equals: false
    },
    {
        _id: 1,
        union: [1, 2, 3, 4, 5, 6],
        intersection: [],
        difference: [1, 2, 3],
        isSubset: false,
        equals: false
    },
    {
        _id: 2,
        union: [1, 2, 3],
        intersection: [],
        difference: [1, 2, 3],
        isSubset: false,
        equals: false
    },
    {_id: 3, union: [4, 5, 6], intersection: [], difference: [], isSubset: true, equals: false},
    {
        _id: 4,
        union: [1, 2, 3],
        intersection: [2, 3],
        difference: [1],
        isSubset: false,
        equals: false
    },
    {_id: 5, union: [2, 3, 4], intersection: [2, 3], difference: [], isSubset: true, equals: false},
    {
        _id: 6,
        union: [1, 2, 3],
        intersection: [1, 2, 3],
        difference: [],
        isSubset: true,
        equals: true
    },
    {
        _id: 7,
        union: [1, 2, 3],
        intersection: [1, 2, 3],
        difference: [],
        isSubset: true,
        equals: true
    },
]);

// No sets to union should produce an empty set for all records so we only check the first one.
assert.eq(coll.aggregate([{$project: {x: {$setUnion: []}}}]).toArray()[0]['x'], []);

// No sets to intersect should produce an empty set for all records so we only check the first one.
assert.eq(coll.aggregate([{$project: {x: {$setIntersection: []}}}]).toArray()[0]['x'], []);
}());
