/**
 * Tests that the validate command detects invalid index options.
 *
 * @tags: [incompatible_with_eft]
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

const checkValidateWarns = function(options = {}) {
    const res = assert.commandWorked(db.runCommand(Object.assign({validate: collName}, options)));
    assert(res.valid, res);
    assert.eq(res.errors.length, 0, res);
    assert.eq(res.warnings.length, 1, res);
    assert(res.warnings[0].includes("contains invalid fields"));
};

// Foreground and background validations can detect invalid index options.
checkValidateWarns();

// Forces a checkpoint to make the background validation see the data.
assert.commandWorked(db.adminCommand({fsync: 1}));
checkValidateWarns({background: true});

// Validating only the metadata can detect invalid index options.
checkValidateWarns({metadata: true});

// Validation of metadata complete for collection. No problems detected.
checkLog.containsJson(conn, 5980500);

// Cannot use { metadata: true } with any other options. Background validation is converted into a
// foreground validation on the ephemeralForTest storage engine, making it incompatible with this
// test.
assert.commandFailedWithCode(db.runCommand({validate: collName, metadata: true, background: true}),
                             ErrorCodes.CommandNotSupported);
assert.commandFailedWithCode(db.runCommand({validate: collName, metadata: true, repair: true}),
                             ErrorCodes.CommandNotSupported);
assert.commandFailedWithCode(db.runCommand({validate: collName, metadata: true, full: true}),
                             ErrorCodes.CommandNotSupported);
assert.commandFailedWithCode(
    db.runCommand({validate: collName, metadata: true, enforceFastCount: true}),
    ErrorCodes.CommandNotSupported);

// Drop the index with the invalid index options and validate only the metadata.
assert.commandWorked(coll.dropIndex({x: 1}));

let validateRes = assert.commandWorked(db.runCommand({validate: collName, metadata: true}));
assert(validateRes.valid);

// Validation of metadata complete for collection. No problems detected.
checkLog.containsJson(conn, 5980500);

MongoRunner.stopMongod(conn);
}());
