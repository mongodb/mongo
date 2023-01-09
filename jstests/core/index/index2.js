/**
 * Test indexing where the key is an embedded object.
 */

(function() {
'use strict';

let t = db.index2_without_index;
t.drop();

assert.eq(t.findOne(), null);

const docs = [
    {_id: 0, name: "foo", z: {a: 17}},
    {_id: 1, name: "foo", z: {a: 17}},
    {_id: 2, name: "barrr", z: {a: 18}},
    {_id: 3, name: "barrr", z: {k: "zzz", L: [1, 2]}},
];

assert.commandWorked(t.insert(docs[0]));
assert.eq(t.findOne().z.a, 17);

// We will reuse these predicates to check the effect of
// additional inserts and indexes on our query results.
assert.commandWorked(t.insert(docs.slice(1, 3)));
assert.eq(t.findOne({z: {a: 17}}).z.a, 17);
assert.eq(t.countDocuments({z: {a: 17}}), 2);
assert.eq(t.countDocuments({z: {a: 18}}), 1);

// Inserting document with key that does not match any of our
// predicates.
assert.commandWorked(t.insert(docs.slice(3)));
assert.eq(t.findOne({z: {a: 17}}).z.a, 17);
assert.eq(t.countDocuments({z: {a: 17}}), 2);
assert.eq(t.countDocuments({z: {a: 18}}), 1);

// Adding an index should not change results.
t = db.index2_with_index;
t.drop();
assert.commandWorked(t.createIndex({z: 1}));
assert.commandWorked(t.insert(docs));
assert.eq(t.findOne({z: {a: 17}}).z.a, 17);
assert.eq(t.countDocuments({z: {a: 17}}), 2);
assert.eq(t.countDocuments({z: {a: 18}}), 1);

// Providing a sort preference should not change resutls.
const sortedDocsAscending = t.find().sort({z: 1});
assert.eq(sortedDocsAscending.length(), 4, tojson(sortedDocsAscending.toArray()));
const sortedDocsDescending = t.find().sort({z: -1});
assert.eq(sortedDocsDescending.length(), 4, tojson(sortedDocsDescending.toArray()));
})();
