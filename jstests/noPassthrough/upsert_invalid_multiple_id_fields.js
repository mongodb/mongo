/**
 * SERVER-75879: Tests that an invalid document with multiple _id fields cannot be inserted by an
 * update sent with upsert=true.
 */
(function() {
"use strict";

// Run tests on a standalone mongod.
let conn = MongoRunner.runMongod({setParameter: {enableComputeMode: true}});
let db = conn.getDB(jsTestName());

// _buildBsonObj is a lightweight BSON builder allowing us to construct an invalid BSON in shell.
let invalidBson = _buildBsonObj("a", 1, "_id", 1, "_id", 2, "_id", 3);

// Assert the BSON is indeed invalid. First, we build a valid one from its JSON string.
let validBson = JSON.parse(JSON.stringify(invalidBson));
assert.eq(JSON.stringify(invalidBson), JSON.stringify(validBson));
assert.gt(Object.bsonsize(invalidBson), Object.bsonsize(validBson));
assert.neq(bsonWoCompare(invalidBson, validBson), 0);

// Test that a replacement is not permitted
assert.throwsWithCode(() => {
    db.coll.replaceOne({}, invalidBson, {upsert: true});
}, 2);

// Test that an upsert is not permitted
assert.writeErrorWithCode(db.coll.update({}, invalidBson, {upsert: true}), ErrorCodes.BadValue);

// Assert that a valid one is actually insertable
assert.writeOK(db.coll.update({}, validBson, {upsert: true}));

let inserted = db.coll.findOne();
assert.docEq(inserted, validBson);
MongoRunner.stopMongod(conn);
})();
