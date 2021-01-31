/**
 * Test the $binarySize expression.
 */
(function() {
"use strict";
load("jstests/aggregation/extras/utils.js");

const coll = db.expression_binarySize;
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0, x: ""},
    {_id: 1, x: "abc"},
    {_id: 2, x: "ab\0c"},
    {_id: 3, x: "abc\0"},
    {_id: 4, x: BinData(0, "")},
    {_id: 5, x: BinData(0, "1234")},
    {_id: 6, x: null},
    {_id: 7},
]));

const result =
    coll.aggregate([{$sort: {_id: 1}}, {$addFields: {s: {$binarySize: "$x"}}}]).toArray();
assert.eq(result, [

    {_id: 0, x: "", s: 0},
    {_id: 1, x: "abc", s: 3},
    // Javascript strings and BSON strings can contain '\0', so both of these have length 4.
    {_id: 2, x: "ab\0c", s: 4},
    {_id: 3, x: "abc\0", s: 4},

    {_id: 4, x: BinData(0, ""), s: 0},
    // The mongo shell BinData constructor takes base64, so "1234" encodes 3 bytes.
    {_id: 5, x: BinData(0, "1234"), s: 3},

    // $binarySize also accepts nullish values, and returns null.
    {_id: 6, x: null, s: null},
    {_id: 7, s: null},
]);

// $binarySize only accepts strings and BinData.
assert.commandWorked(coll.insert({x: 42}));
assertErrorCode(coll, {$project: {s: {$binarySize: "$x"}}}, 51276);
}());
