// Test $in regular expressions with a mix of types.
(function() {
'use strict';

const coll = db.jstests_in_with_mixed_values;

// Exercise mixed regex and scalar integer cases.
let docs = [
    {x: 1},
    {x: 2},
    {x: 3},
    {x: 'ab'},
    {x: 'ac'},
    {x: 'ad'},
];
assert.commandWorked(coll.insert(docs));

assert.eq(4, coll.find({x: {$in: [1, /^a/]}}).itcount());
assert.eq(4, coll.find({x: {$in: [/^a/, 1]}}).itcount());
assert.eq(5, coll.find({x: {$in: [/^a/, 1, 2]}}).itcount());
assert.eq(6, coll.find({x: {$in: [/^a/, 1, 2, 3]}}).itcount());
assert.eq(4, coll.find({x: {$in: [/^ab/, 1, 2, 3]}}).itcount());
assert.eq(5, coll.find({x: {$in: [/^a/, 1, 2, /^ab/]}}).itcount());
assert.eq(5, coll.find({x: {$in: [1, /^ab/, 2, /^a/]}}).itcount());
assert.eq(6, coll.find({x: {$in: [/^a/, 1, 2, 3, /^ab/]}}).itcount());
assert.eq(6, coll.find({x: {$in: [1, /^ab/, 2, 3, /^a/]}}).itcount());
assert(coll.drop());

// Exercise mixed regex and composite type cases.
docs = [
    {x: 1},
    {x: 2},
    {x: 3},
    {x: 'ab'},
    {x: 'ac'},
    {x: 'ad'},
    {x: [1]},
    {x: [1, 2]},
    {x: [1, 'ab']},
    {x: ['ac', 2]},
    {x: {y: 1}},
    {x: {y: 'ab'}},
    {x: {y: [99, {z: 1}]}}
];
assert.commandWorked(coll.insert(docs));

assert.eq(8, coll.find({x: {$in: [1, /^a/]}}).itcount());
assert.eq(7, coll.find({x: {$in: [2, /^a/]}}).itcount());
assert.eq(6, coll.find({x: {$in: [[1, 2], /^a/]}}).itcount());
assert.eq(5, coll.find({x: {$in: [/^a/]}}).itcount());
assert.eq(1, coll.find({'x.y': {$in: [1, 2]}}).itcount());
assert.eq(2, coll.find({'x.y': {$in: [1, /^a/]}}).itcount());
assert.eq(1, coll.find({'x': {$in: [{y: 1}, /^z/]}}).itcount());
assert.eq(0, coll.find({'x': {$in: [{z: 1}, /^z/]}}).itcount());
assert.eq(0, coll.find({'x': {$in: [{y: 99}, /^z/]}}).itcount());
assert.eq(1, coll.find({'x.y': {$in: [{z: 10}, {z: 1}, /^z/]}}).itcount());
assert.eq(0, coll.find({'x.y': {$in: [{z: 10}, {z: 11}, /^z/]}}).itcount());

// Exercise dotted field paths with mixed scalar integers, strings and regular expressions.
docs = [
    {x: 1, y: {z: 1}},
    {x: 1, y: {z: 2}},
    {x: 1, y: {z: 3}},
    {x: 2, y: {z: 'ab'}},
    {x: 2, y: {z: 'ac'}},
    {x: 2, y: {z: 'ad'}},
];
assert.commandWorked(coll.insert(docs));

assert.eq(4, coll.find({'y.z': {$in: [1, /^a/]}}).itcount());
assert.eq(4, coll.find({'y.z': {$in: [/^a/, 1]}}).itcount());
assert.eq(2, coll.find({'y.z': {$in: [1, /^ab/]}}).itcount());
assert.eq(3, coll.find({'y.z': {$in: [1, 2, /^ab/]}}).itcount());
assert.eq(4, coll.find({'y.z': {$in: [1, 2, 3, /^ab/]}}).itcount());
assert.eq(5, coll.find({'y.z': {$in: [1, 2, 3, /^ab/, /^ac/]}}).itcount());
assert.eq(6, coll.find({'y.z': {$in: [1, 2, 3, /^a/]}}).itcount());
assert.eq(6, coll.find({'y.z': {$in: [/^a/, 1, 2, 3]}}).itcount());
assert.eq(6, coll.find({'y.z': {$in: ["ab", "ac", "ad", 1, 2, 3]}}).itcount());
assert.eq(5, coll.find({'y.z': {$in: ["ac", "ad", 1, 2, 3]}}).itcount());
assert.eq(4, coll.find({'y.z': {$in: ["ad", 1, 2, 3]}}).itcount());
assert.eq(3, coll.find({'y.z': {$in: ["ad", /^a/]}}).itcount());
assert.eq(0, coll.find({'y.z': {$in: ["foo", 999]}}).itcount());
assert.eq(0, coll.find({'y.z': {$in: [999, /no/]}}).itcount());
assert.eq(0, coll.find({'y.z': {$in: ["bar", /0-9/]}}).itcount());
assert(coll.drop());

// Exercise binary and other types.
docs = [
    {x: 1},
    {x: "a"},
    {x: [1, 2]},
    {x: Infinity},
    {x: NaN},
    {x: BinData(0, "AAA=")},
    {x: BinData(1, "AAA=")},
    {x: BinData(2, "AAA=")},
    {x: BinData(0, "ZAA=")},
    {x: BinData(1, "ZAA=")},
    {x: BinData(2, "ZAA=")},
    {x: BinData(0, "VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wcyBvdmVyIHRoZSBsYXp5IGRvZw==")},
    {x: BinData(1, "VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wcyBvdmVyIHRoZSBsYXp5IGRvZw==")},
    {x: BinData(2, "VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wcyBvdmVyIHRoZSBsYXp5IGRvZw==")},
];
assert.commandWorked(coll.insert(docs));

assert.eq(3, coll.find({x: {$in: [1, BinData(0, "AAA=")]}}).itcount());
assert.eq(2, coll.find({x: {$in: [[1, 2], BinData(0, "AAA=")]}}).itcount());
assert.eq(2, coll.find({x: {$in: [/^a/, BinData(1, "AAA=")]}}).itcount());
assert.eq(1, coll.find({x: {$in: ["b", BinData(1, "AAA=")]}}).itcount());
assert.eq(2, coll.find({x: {$in: [BinData(0, "ZAA="), BinData(0, "AAA=")]}}).itcount());
assert.eq(2, coll.find({x: {$in: [BinData(1, "ZAA="), BinData(0, "AAA=")]}}).itcount());
assert.eq(2, coll.find({x: {$in: [BinData(0, "ZAA="), BinData(1, "AAA=")]}}).itcount());
assert.eq(0, coll.find({x: {$in: [BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")]}}).itcount());

assert(coll.drop());
})();
