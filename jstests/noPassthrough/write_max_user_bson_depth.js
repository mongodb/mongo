/**
 * Tests that documents with depth equal to the max user BSON depth can be inserted, and that
 * existing documents can be updated to have depth equal to the max user BSON depth. In particular,
 * tests that empty subdocuments do not count toward the depth of a document.
 */
(function() {
'use strict';

// Max user BSON depth is 20 less than the max absolute BSON depth.
const conn = MongoRunner.runMongod({setParameter: {maxBSONDepth: 21}});

const coll = conn.getDB('test')[jsTestName()];

// Can insert a document with depth equal to the max user BSON depth.
assert.commandWorked(coll.insert({a: {}}));

// Cannot insert a document with depth greater than the max user BSON depth.
assert.commandFailedWithCode(coll.insert({a: {b: 1}}), ErrorCodes.Overflow);

// Can update a document to have depth equal to the max user BSON depth.
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.update({a: 1}, {$set: {a: {}}}));

// Cannot update a document to have depth greater than the max user BSON depth.
assert.commandFailedWithCode(coll.update({}, {$set: {a: {b: 1}}}), ErrorCodes.Overflow);

MongoRunner.stopMongod(conn);
})();
