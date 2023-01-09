// Test that $setWindowFields inside $facet correctly propagates its state when it encounters paused
// execution.
(function() {
"use strict";

const coll = db.window_inside_facet;
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 'a', n: 0},
    {_id: 'b', n: 1},
    {_id: 'c', n: 2},
    {_id: 'd', n: 3},
    {_id: 'e', n: 4},
    {_id: 'f', n: 5}
]));

// Test window with a sort within $facet alongside another pipeline that will cause it to pause
// execution. The sort will cause the window to hit paused execution immediately, before an advance.
let result =
    coll.aggregate({
            $facet: {
                facet1: [{
                    $setWindowFields: {
                        output: {prevId: {$shift: {by: -1, default: null, output: "$_id"}}},
                        sortBy: {_id: 1}
                    }
                }],
                facet2: [{$count: "count"}]
            }
        })
        .toArray()[0];
let expected = {
    facet1: [
        {_id: 'a', n: 0, prevId: null},
        {_id: 'b', n: 1, prevId: 'a'},
        {_id: 'c', n: 2, prevId: 'b'},
        {_id: 'd', n: 3, prevId: 'c'},
        {_id: 'e', n: 4, prevId: 'd'},
        {_id: 'f', n: 5, prevId: 'e'}
    ],
    facet2: [{count: 6}]
};
assert.docEq(expected, result, "$setWindowFields with sort failed.");

// Test window with no sort within $facet alongside another pipeline that will cause it to pause
// execution. Having no sort will cause the window to hit paused execution after advancing.
result = coll.aggregate({
                 $facet: {
                     facet1: [
                         {
                             $setWindowFields: {
                                 output: {
                                     min: {$min: "$n"},
                                     max: {$max: "$n"},
                                 }
                             }
                         },
                         {$sort: {_id: 1}}
                     ],
                     facet2: [{$count: "count"}]
                 }
             })
             .toArray()[0];
expected = {
    facet1: [
        {_id: 'a', n: 0, min: 0, max: 5},
        {_id: 'b', n: 1, min: 0, max: 5},
        {_id: 'c', n: 2, min: 0, max: 5},
        {_id: 'd', n: 3, min: 0, max: 5},
        {_id: 'e', n: 4, min: 0, max: 5},
        {_id: 'f', n: 5, min: 0, max: 5}
    ],
    facet2: [{count: 6}]
};
assert.docEq(expected, result, "$setWindowFields without sort failed.");
}());
