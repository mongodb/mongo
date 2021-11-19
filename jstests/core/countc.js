// In fast count mode the Matcher is bypassed when matching can be performed by a BtreeCursor and
// its delegate FieldRangeVector or an IntervalBtreeCursor.  The tests below check that fast count
// mode is implemented appropriately in specific cases.
//
// SERVER-1752

// @tags: [
//     # Uses $where operator
//     requires_scripting
// ]

(function() {
'use strict';
const collNamePrefix = 'jstests_countc_';
let collCount = 0;
let t = db.getCollection(collNamePrefix + collCount++);
t.drop();

// Match a subset of inserted values within a $in operator.
assert.commandWorked(t.createIndex({a: 1}));
// Save 'a' values 0, 0.5, 1.5, 2.5 ... 97.5, 98.5, 99.
let docs = [];
let docId = 0;
docs.push({_id: docId++, a: 0});
docs.push({_id: docId++, a: 99});
for (let i = 0; i < 99; ++i) {
    docs.push({_id: docId++, a: (i + 0.5)});
}
assert.commandWorked(t.insert(docs));

// Query 'a' values $in 0, 1, 2, ..., 99.
let vals = [];
for (let i = 0; i < 100; ++i) {
    vals.push(i);
}
// Only values 0 and 99 of the $in set are present in the collection, so the expected count is 2.
assert.eq(2, t.count({a: {$in: vals}}));

// Match 'a' values within upper and lower limits.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.insert([
    {_id: docId++, a: [1, 2]},  // Will match because 'a' is in range.
    {_id: docId++, a: 9},       // Will not match because 'a' is not in range.
]));
// Only one document matches.
assert.eq(1, t.count({a: {$gt: 0, $lt: 5}}));

// Match two nested fields within an array.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.createIndex({'a.b': 1, 'a.c': 1}));
assert.commandWorked(t.insert({a: [{b: 2, c: 3}, {}]}));
// The document does not match because its c value is 3.
assert.eq(0, t.count({'a.b': 2, 'a.c': 2}));

// $gt:string only matches strings.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.insert([
    {_id: docId++, a: 'a'},  // Will match.
    {_id: docId++, a: {}},   // Will not match because {} is not a string.
]));
// Only one document matches.
assert.eq(1, t.count({a: {$gte: ''}}));

// $lte:date only matches dates.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.insert([
    {_id: docId++, a: new Date(1)},  // Will match.
    {_id: docId++, a: true},         // Will not match because 'true' is not a date.
]));
// Only one document matches.
assert.eq(1, t.count({a: {$lte: new Date(1)}}));

// Querying for 'undefined' triggers an error.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.createIndex({a: 1}));
assert.throwsWithCode(function() {
    t.count({a: undefined});
}, ErrorCodes.BadValue);

// Count using a descending order index.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.createIndex({a: -1}));
assert.commandWorked(t.insert([
    {_id: docId++, a: 1},
    {_id: docId++, a: 2},
    {_id: docId++, a: 3},
]));
assert.eq(1, t.count({a: {$gt: 2}}));
assert.eq(1, t.count({a: {$lt: 2}}));
assert.eq(2, t.count({a: {$lte: 2}}));
assert.eq(2, t.count({a: {$lt: 3}}));

// Count using a compound index.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.createIndex({a: 1, b: 1}));
assert.commandWorked(t.insert([
    {_id: docId++, a: 1, b: 2},
    {_id: docId++, a: 2, b: 1},
    {_id: docId++, a: 2, b: 3},
    {_id: docId++, a: 3, b: 4},
]));
assert.eq(1, t.count({a: 1}));
assert.eq(2, t.count({a: 2}));
assert.eq(1, t.count({a: {$gt: 2}}));
assert.eq(1, t.count({a: {$lt: 2}}));
assert.eq(2, t.count({a: 2, b: {$gt: 0}}));
assert.eq(1, t.count({a: 2, b: {$lt: 3}}));
assert.eq(1, t.count({a: 1, b: {$lt: 3}}));

// Count using a compound descending order index.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.createIndex({a: 1, b: -1}));
assert.commandWorked(t.insert([
    {_id: docId++, a: 1, b: 2},
    {_id: docId++, a: 2, b: 1},
    {_id: docId++, a: 2, b: 3},
    {_id: docId++, a: 3, b: 4},
]));
assert.eq(1, t.count({a: {$gt: 2}}));
assert.eq(1, t.count({a: {$lt: 2}}));
assert.eq(2, t.count({a: 2, b: {$gt: 0}}));
assert.eq(1, t.count({a: 2, b: {$lt: 3}}));
assert.eq(1, t.count({a: 1, b: {$lt: 3}}));

// Count with a multikey value.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.insert({_id: docId++, a: [1, 2]}));
assert.eq(1, t.count({a: {$gt: 0, $lte: 2}}));

// Count with a match constraint on an unindexed field.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.insert([
    {_id: docId++, a: 1, b: 1},
    {_id: docId++, a: 1, b: 2},
]));
assert.eq(1, t.count({a: 1, $where: 'this.b == 1'}));
})();
