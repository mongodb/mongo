// Tests that validators with encryption-related keywords and action "warn" correctly logs a startup
// warning after upgrade and is not usable. Also verify that we can collMod the validator such that
// the collection is then usable.
// @tags: [requires_majority_read_concern]
(function() {
"use strict";

load("jstests/multiVersion/libs/multi_rs.js");  // For upgradeSet.

const preBackport44Version = "4.4.0";
const latestVersion = "latest";

const encryptSchema = {
    $jsonSchema: {properties: {_id: {encrypt: {}}}}
};

const rst = new ReplSetTest({
    nodes: 3,
    nodeOptions: {binVersion: preBackport44Version},
});
rst.startSet();
rst.initiate();

// Up- or downgrades the replset and then refreshes our references to the test collection.
function refreshReplSet(version) {
    // Upgrade the set and wait for it to become available again.
    rst.upgradeSet({binVersion: version});
    rst.awaitSecondaryNodes();

    // Having upgraded the set, reacquire references to the db and collection.
    testDB = rst.getPrimary().getDB(jsTestName());
    coll = testDB[collName];
}

// Obtain references to the test database and create the test collection.
let testDB = rst.getPrimary().getDB(jsTestName());
let collName = "doc_validation_encrypt_keywords";
let coll = testDB[collName];
coll.drop();

assert.commandWorked(
    testDB.createCollection(collName, {validator: encryptSchema, validationAction: "warn"}));

// Check that an insert which violates the validator passes.
assert.commandWorked(coll.insert({_id: 0}));

// Upgrade the replica set to 'latest'.
refreshReplSet("latest");

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

rst.stopSet();
})();
