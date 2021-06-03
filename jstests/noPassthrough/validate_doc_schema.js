/**
 * Tests that the validate command reports documents not adhering to collection schema rules.
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod();

const dbName = "test";
const collName = "validate_doc_schema";

const db = conn.getDB(dbName);

assert.commandWorked(db.createCollection(collName, {validator: {a: {$exists: true}}}));
const coll = db.getCollection(collName);

assert.commandWorked(db.runCommand(
    {insert: collName, documents: [{a: 1}, {b: 1}, {c: 1}], bypassDocumentValidation: true}));

// Validation detects documents not adhering to the collection schema rules.
let res = assert.commandWorked(coll.validate());
assert(res.valid);

// Even though there are two documents violating the collection schema rules, the warning should
// only be shown once.
assert.eq(res.warnings.length, 1);

checkLog.containsJson(conn, 5363500, {recordId: "2"});
checkLog.containsJson(conn, 5363500, {recordId: "3"});

// Remove the documents violating the collection schema rules.
assert.commandWorked(coll.remove({b: 1}));
assert.commandWorked(coll.remove({c: 1}));

res = assert.commandWorked(coll.validate());
assert(res.valid);
assert.eq(res.warnings.length, 0);

MongoRunner.stopMongod(conn);
}());
