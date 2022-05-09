/**
 * Tests that in 6.1 version listIndexes can parse invalid index specs created before 5.0 version.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load('jstests/multiVersion/libs/multi_rs.js');

var nodes = {
    n1: {binVersion: "4.4"},
    n2: {binVersion: "4.4"},
};

var rst = new ReplSetTest({nodes: nodes});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = jsTestName();

let primaryDB = rst.getPrimary().getDB(dbName);
let primaryColl = primaryDB.getCollection(collName);

// In earlier versions, users were able to add invalid index options when creating an index. The
// option could still be interpreted accordingly.
assert.commandWorked(primaryColl.createIndex({x: 1}, {sparse: "yes"}));

// Upgrades from 4.4 to 5.0.
jsTestLog("Upgrading to version 5.0");
rst.upgradeSet({binVersion: "5.0"});
assert.commandWorked(rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: "5.0"}));

// Upgrades from 5.0 to 6.0.
jsTestLog("Upgrading to version last-lts");
rst.upgradeSet({binVersion: "last-lts"});
assert.commandWorked(rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

// Upgrades from 6.0 to latest.
jsTestLog("Upgrading to version latest");
rst.upgradeSet({binVersion: "latest"});
const primary = rst.getPrimary();
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

primaryDB = primary.getDB(dbName);

// Verify listIndexes command can correctly output the repaired index specs.
assert.commandWorked(primaryDB.runCommand({listIndexes: collName}));

// Add a new node to make sure the initial sync works correctly with the invalid index specs.
jsTestLog("Bringing up a new node");
rst.add();
rst.reInitiate();

jsTestLog("Waiting for new node to be synced.");
rst.awaitReplication();
rst.awaitSecondaryNodes();

const [secondary1, secondary2] = rst.getSecondaries();
const secondaryDB1 = secondary1.getDB(dbName);
const secondaryDB2 = secondary2.getDB(dbName);

// Verify that the existing nodes detect invalid index options, but the new node has the repaired
// index spec.
let validateRes = assert.commandWorked(primaryDB.runCommand({validate: collName}));
assert(!validateRes.valid, "validate should fail: " + tojson(validateRes));

validateRes = assert.commandWorked(secondaryDB1.runCommand({validate: collName}));
assert(!validateRes.valid, "validate should fail: " + tojson(validateRes));

validateRes = assert.commandWorked(secondaryDB2.runCommand({validate: collName}));
assert(validateRes.valid, "validate should succeed: " + tojson(validateRes));

// Use collMod to fix the invalid index options in the collection.
assert.commandWorked(primaryDB.runCommand({collMod: collName}));

// Fix the invalid fields from index spec.
checkLog.containsJson(primary, 6444400, {fieldName: "sparse"});
checkLog.containsJson(secondary1, 6444400, {fieldName: "sparse"});

// Verify that the index no longer has invalid index options.
assert.commandWorked(primaryDB.runCommand({listIndexes: collName}));

validateRes = assert.commandWorked(primaryDB.runCommand({validate: collName}));
assert(validateRes.valid, "validate should succeed: " + tojson(validateRes));

validateRes = assert.commandWorked(secondaryDB1.runCommand({validate: collName}));
assert(validateRes.valid, "validate should succeed: " + tojson(validateRes));

validateRes = assert.commandWorked(secondaryDB2.runCommand({validate: collName}));
assert(validateRes.valid, "validate should succeed: " + tojson(validateRes));

rst.stopSet();
})();