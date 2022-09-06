/**
 * Tests that the validate command detects invalid index options.
 *
 * @tags: [requires_fsync, requires_wiredtiger, requires_persistence]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const conn = MongoRunner.runMongod();

const dbName = "test";
const collName = jsTestName();

const db = conn.getDB(dbName);
const coll = db.getCollection(collName);

// In earlier versions of the server, users were able to add invalid index options when creating an
// index. This fail point allows us to skip validating index options to simulate the old behaviour.
const fp = configureFailPoint(db, "skipIndexCreateFieldNameValidation");

// "safe" and "force" are invalid index options.
assert.commandWorked(coll.createIndex({x: 1}, {safe: true, sparse: true, force: false}));

fp.off();

// Foreground and background validations can detect invalid index options.
let validateRes = assert.commandWorked(db.runCommand({validate: collName}));
assert(!validateRes.valid);

// Forces a checkpoint to make the background validation see the data.
assert.commandWorked(db.adminCommand({fsync: 1}));
validateRes = assert.commandWorked(db.runCommand({validate: collName, background: true}));
assert(!validateRes.valid);

// Validating only the metadata can detect invalid index options.
validateRes = assert.commandWorked(db.runCommand({validate: collName, metadata: true}));
assert(!validateRes.valid);

// Validation of metadata complete for collection. Problems detected.
checkLog.containsJson(conn, 5980501);

// Cannot use { metadata: true } with any other options.
assert.commandFailedWithCode(db.runCommand({validate: collName, metadata: true, background: true}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(db.runCommand({validate: collName, metadata: true, repair: true}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(db.runCommand({validate: collName, metadata: true, full: true}),
                             ErrorCodes.InvalidOptions);
assert.commandFailedWithCode(
    db.runCommand({validate: collName, metadata: true, enforceFastCount: true}),
    ErrorCodes.InvalidOptions);

// Drop the index with the invalid index options and validate only the metadata.
assert.commandWorked(coll.dropIndex({x: 1}));

validateRes = assert.commandWorked(db.runCommand({validate: collName, metadata: true}));
assert(validateRes.valid);

// Validation of metadata complete for collection. No problems detected.
checkLog.containsJson(conn, 5980500);

MongoRunner.stopMongod(conn);
}());
