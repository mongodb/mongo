// $or clause deduping with result set sizes > 101 (smaller result sets are now also deduped by the
// query optimizer cursor).

(function() {
'use strict';

const collNamePrefix = 'jstests_orp_';
let collCount = 0;
let docId = 0;

let t = db.getCollection(collNamePrefix + collCount++);
t.drop();

assert.commandWorked(t.createIndexes([{a: 1}, {b: 1}, {c: 1}]));

let docs = [];
for (let i = 0; i < 110; ++i) {
    docs.push({_id: docId++, a: 1, b: 1});
}
assert.commandWorked(t.insert(docs));

// Deduping results from the previous clause.
assert.eq(docs.length, t.countDocuments({$or: [{a: 1}, {b: 1}]}));

// Deduping results from a prior clause.
assert.eq(docs.length, t.countDocuments({$or: [{a: 1}, {c: 1}, {b: 1}]}));
assert.commandWorked(t.insert({_id: docId++, c: 1}));
assert.eq(docs.length + 1, t.countDocuments({$or: [{a: 1}, {c: 1}, {b: 1}]}));

// Deduping results that would normally be index only matches on overlapping and double scanned $or
// field regions.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.createIndex({a: 1, b: 1}));
docs = [];
let k = 11;
for (let i = 0; i < k; ++i) {
    for (let j = 0; j < k; ++j) {
        docs.push({_id: docId++, a: i, b: j});
    }
}
assert.commandWorked(t.insert(docs));
assert.eq(k * k,
          t.countDocuments({$or: [{a: {$gte: 0}, b: {$gte: 0}}, {a: {$lte: k}, b: {$lte: k}}]}));

// Deduping results from a clause that completed before the multi cursor takeover.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.createIndexes([{a: 1}, {b: 1}]));
docs = [];
k = 120;
docs.push({_id: docId++, a: 1, b: k});
for (let i = 0; i < k; ++i) {
    docs.push({_id: docId++, b: i});
}
assert.commandWorked(t.insert(docs));
assert.eq(k + 1, t.countDocuments({$or: [{a: 1}, {b: {$gte: 0}}]}));
})();
