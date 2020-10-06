/**
 * Tests behavior of $first and $last aggregation operators.
 */
(function() {
"use strict";

const coll = db.expression_first_last;

coll.drop();
assert.commandWorked(coll.insert([
    {_id: 0, a: []},
    {_id: 1, a: ['A']},
    {_id: 2, a: ['A', 'B']},
    {_id: 3, a: ['A', 'B', 'C']},
    {_id: 4, a: null},
    {_id: 5, a: undefined},
    {_id: 6},
]));

const result =
    coll.aggregate([{$sort: {_id: 1}}, {$project: {f: {$first: "$a"}, l: {$last: "$a"}}}])
        .toArray();
assert.eq(result, [
    // When an array doesn't contain a given index, the result is 'missing', similar to looking up a
    // nonexistent key in a document.
    {_id: 0},

    {_id: 1, f: 'A', l: 'A'},
    {_id: 2, f: 'A', l: 'B'},
    {_id: 3, f: 'A', l: 'C'},

    // When the input is nullish instead of an array, the result is null.
    {_id: 4, f: null, l: null},
    {_id: 5, f: null, l: null},
    {_id: 6, f: null, l: null},
]);
}());
