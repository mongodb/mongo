/**
 * @tags: [requires_fastcount]
 * Tests the countDocuments and estimatedDocumentCount commands.
 */
(function() {
"use strict";

const standalone = MongoRunner.runMongod();
const dbName = "test";
const db = standalone.getDB(dbName);
const collName = "document_count_functions";
const coll = db.getCollection(collName);

coll.drop();

assert.commandWorked(coll.insert({i: 1, j: 1}));
assert.commandWorked(coll.insert({i: 2, j: 1}));
assert.commandWorked(coll.insert({i: 2, j: 2}));

// Base case: Pass a valid query into countDocuments without any extra options.
assert.eq(1, coll.countDocuments({i: 1}));
assert.eq(2, coll.countDocuments({i: 2}));

// Base case: Call estimatedDocumentCount without any extra options.
assert.eq(3, coll.estimatedDocumentCount());

assert.commandWorked(coll.insert({i: 1, j: 2}));
assert.commandWorked(coll.insert({i: 1, j: 3}));
assert.commandWorked(coll.insert({i: 1, j: 4}));

// Limit case: Limit the number of documents to count. There are 4 {i: 1} documents,
// but we will set the limit to 3.
assert.eq(3, coll.countDocuments({i: 1}, {limit: 3}));

// Skip case: Skip a certain number of documents for the count. We will skip 2, meaning
// that we will have 2 left.
assert.eq(2, coll.countDocuments({i: 1}, {skip: 2}));

assert.commandWorked(coll.ensureIndex({i: 1}));

// Aggregate stage case: Add an option that gets added as an aggregation argument.
assert.eq(4, coll.countDocuments({i: 1}, {hint: {i: 1}}));

// Set fail point to make sure estimatedDocumentCount times out.
assert.commandWorked(
    db.adminCommand({configureFailPoint: 'maxTimeAlwaysTimeOut', mode: 'alwaysOn'}));

// maxTimeMS case: Expect an error if an operation times out.
assert.commandFailedWithCode(assert.throws(function() {
                                              coll.estimatedDocumentCount({maxTimeMS: 100});
                                          }),
                                          ErrorCodes.MaxTimeMSExpired);

// Disable fail point.
assert.commandWorked(db.adminCommand({configureFailPoint: 'maxTimeAlwaysTimeOut', mode: 'off'}));

MongoRunner.stopMongod(standalone);
})();