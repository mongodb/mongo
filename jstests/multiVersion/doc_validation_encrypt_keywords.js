/**
 * Tests that validators with encryption-related keywords and action "warn" correctly logs a startup
 * warning after upgrade and is not usable. Also verify that we can collMod the validator such that
 * the collection is then usable.
 *
 * We restart mongod during the test and expect it to have the same data after restarting.
 * @tags: [requires_persistence]
 */
(function() {
"use strict";

const preBackport42Version = "4.2.1";
const latestVersion = "latest";

const encryptSchema = {
    $jsonSchema: {properties: {_id: {encrypt: {}}}}
};

const dbpath = MongoRunner.dataPath + jsTestName();
let conn = MongoRunner.runMongod({binVersion: preBackport42Version, dbpath: dbpath});
assert.neq(null, conn, "mongod was unable to start up");

let testDB = conn.getDB("test");
let collName = "doc_validation_encrypt_keywords";
let coll = testDB[collName];
coll.drop();

assert.commandWorked(
    testDB.createCollection(collName, {validator: encryptSchema, validationAction: "warn"}));

// Check that an insert which violates the validator passes.
assert.commandWorked(coll.insert({_id: 0}));

// Restart the mongod with binVersion "latest".
MongoRunner.stopMongod(conn);
conn = MongoRunner.runMongod({noCleanData: true, binVersion: "latest", dbpath: dbpath});
assert.neq(null, conn, "mongod was unable to start up");

testDB = conn.getDB("test");
coll = testDB[collName];

// Check that we logged a startup warning.
const cmdRes = assert.commandWorked(testDB.adminCommand({getLog: "startupWarnings"}));
assert(/has malformed validator/.test(cmdRes.log));

// Test that inserts to the collection with a disallowed validator is not allowed, even if they pass
// the validator expression.
assert.commandFailedWithCode(coll.insert({_id: BinData(6, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")}),
                             ErrorCodes.QueryFeatureNotAllowed);

// Now collMod the validator to change the action to "strict".
assert.commandWorked(testDB.runCommand({collMod: collName, validationAction: "error"}));

// Retry the insert and verify that it now passes.
assert.commandWorked(coll.insert({_id: BinData(6, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA")}));

// Creating a new collection with a disallowed validator should fail.
assert.commandFailedWithCode(
    testDB.createCollection(collName + "_new",
                            {validator: encryptSchema, validationAction: "warn"}),
    ErrorCodes.QueryFeatureNotAllowed);

MongoRunner.stopMongod(conn);
})();
