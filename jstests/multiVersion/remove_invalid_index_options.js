/**
 * Tests that removing invalid index options in collMod has no negative consequences for replica
 * sets running with earlier versions of the server.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

var nodes = {
    n1: {binVersion: "latest"},
    n2: {binVersion: "last-lts"},
};

var rst = new ReplSetTest({nodes: nodes});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = jsTestName();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

const secondaryDB = secondary.getDB(dbName);

// In earlier versions of the server, users were able to add invalid index options when creating an
// index. This fail point allows us to skip validating index options to simulate the old behaviour.
const fpPrimary = configureFailPoint(primaryDB, "skipIndexCreateFieldNameValidation");
const fpSecondary = configureFailPoint(secondaryDB, "skipIndexCreateFieldNameValidation");

// Create indexes with invalid options.
assert.commandWorked(primaryColl.createIndex({x: 1}, {safe: true, sparse: true, force: false}));
assert.commandWorked(primaryColl.createIndex({y: 1}, {sparse: true}));
assert.commandWorked(primaryColl.createIndex({z: 1}, {xyz: false}));

fpPrimary.off();
fpSecondary.off();

// Verify that the primary (latest) and secondary (last-lts) detect invalid index options.
let validateRes = assert.commandWorked(primaryDB.runCommand({validate: collName}));
assert(!validateRes.valid);

validateRes = assert.commandWorked(secondaryDB.runCommand({validate: collName}));
assert(!validateRes.valid);

// Use collMod to remove the invalid index options in the collection.
assert.commandWorked(primaryDB.runCommand({collMod: collName}));

// Removing unknown field from index spec.
checkLog.containsJson(primary, 23878, {fieldName: "safe"});
checkLog.containsJson(primary, 23878, {fieldName: "force"});
checkLog.containsJson(primary, 23878, {fieldName: "xyz"});

checkLog.containsJson(secondary, 23878, {fieldName: "safe"});
checkLog.containsJson(secondary, 23878, {fieldName: "force"});
checkLog.containsJson(secondary, 23878, {fieldName: "xyz"});

// Verify that the index no longer has invalid index options.
validateRes = assert.commandWorked(primaryDB.runCommand({validate: collName}));
assert(validateRes.valid);

validateRes = assert.commandWorked(secondaryDB.runCommand({validate: collName}));
assert(validateRes.valid);

rst.stopSet();
})();
