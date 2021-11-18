/**
 * Tests that we can create indexes that are multikey.
 */

(function() {
"use strict";
const collNamePrefix = 'background_index_multikey__';
let collCount = 0;

// Build index after multikey document is in the collection.
let coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));
let doc = {_id: 0, a: [1, 2]};
assert.commandWorked(coll.insert(doc));
assert.sameMembers([doc], coll.find({a: 1}).toArray());
assert.sameMembers([doc], coll.find({a: 2}).toArray());

// Build index where multikey is in an embedded document.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({'b.c': 1}));
doc = {
    _id: 1,
    b: {c: [1, 2]}
};
assert.commandWorked(coll.insert(doc));
assert.sameMembers([doc], coll.find({'b.c': 1}).toArray());
assert.sameMembers([doc], coll.find({'b.c': 2}).toArray());

// Add new multikey path to embedded path.
doc = {
    _id: 2,
    b: [1, 2]
};
assert.commandWorked(coll.insert(doc));
assert.sameMembers([doc], coll.find({b: 1}).toArray());
assert.sameMembers([doc], coll.find({b: 2}).toArray());

// Build index on a large collection that is not multikey, and then make it multikey.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({d: 1}));
let docs = [];
for (let i = 100; i < 1100; i++) {
    docs.push({_id: i, d: i});
}
assert.commandWorked(coll.insert(docs));
doc = {
    _id: 3,
    d: [1, 2]
};
assert.commandWorked(coll.insert(doc));
assert.sameMembers([doc], coll.find({d: 1}).toArray());
assert.sameMembers([doc], coll.find({d: 2}).toArray());

// Build compound multikey index.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({e: 1, f: 1}));
doc = {
    _id: 4,
    e: [1, 2]
};
assert.commandWorked(coll.insert(doc));
assert.sameMembers([doc], coll.find({e: 1}).toArray());
assert.sameMembers([doc], coll.find({e: 2}).toArray());

// Add new multikey path to compound index.
doc = {
    _id: 5,
    f: [1, 2]
};
assert.commandWorked(coll.insert(doc));
assert.sameMembers([doc], coll.find({f: 1}).toArray());
assert.sameMembers([doc], coll.find({f: 2}).toArray());
})();
