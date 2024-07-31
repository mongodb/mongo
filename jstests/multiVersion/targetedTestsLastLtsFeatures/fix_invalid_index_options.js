/**
 * Tests that in 5.0 version collMod fixes invalid index specs created before 5.0 version.
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

let secondaryDB = rst.getSecondary().getDB(dbName);

// In earlier versions, users were able to add invalid index options when creating an index.
assert.commandWorked(primaryColl.createIndex({x: 1}, {sparse: "yes"}));

// Upgrades from 4.4 to 5.0.
jsTestLog("Upgrading to version latest");
rst.upgradeSet({binVersion: "latest"});
const primary = rst.getPrimary();
const secondary = rst.getSecondary();
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

primaryDB = primary.getDB(dbName);
secondaryDB = secondary.getDB(dbName);

// Verify that the primary and secondary in 6.0 detect invalid index options.
let validateRes = assert.commandWorked(primaryDB.runCommand({validate: collName}));
assert(validateRes.valid, "validate should succeed: " + tojson(validateRes));
assert.eq(validateRes.errors.length, 0, "validate should not error: " + tojson(validateRes));
assert.eq(validateRes.warnings.length, 1, "validate should warn: " + tojson(validateRes));

validateRes = assert.commandWorked(secondaryDB.runCommand({validate: collName}));
assert(validateRes.valid, "validate should succeed: " + tojson(validateRes));
assert.eq(validateRes.errors.length, 0, "validate should not error: " + tojson(validateRes));
assert.eq(validateRes.warnings.length, 1, "validate should warn: " + tojson(validateRes));

// Use collMod to fix the invalid index options in the collection.
assert.commandWorked(primaryDB.runCommand({collMod: collName}));

// Fix invalid field from index spec.
checkLog.containsJson(primary, 6444400, {fieldName: "sparse"});
checkLog.containsJson(secondary, 6444400, {fieldName: "sparse"});

// Verify that the index no longer has invalid index options.
assert.commandWorked(primaryDB.runCommand({listIndexes: collName}));

validateRes = assert.commandWorked(primaryDB.runCommand({validate: collName}));
assert(validateRes.valid, "validate should succeed: " + tojson(validateRes));
assert.eq(validateRes.errors.length, 0, "validate should not error: " + tojson(validateRes));
assert.eq(validateRes.warnings.length, 0, "validate should not warn: " + tojson(validateRes));

validateRes = assert.commandWorked(secondaryDB.runCommand({validate: collName}));
assert(validateRes.valid, "validate should succeed: " + tojson(validateRes));
assert.eq(validateRes.errors.length, 0, "validate should not error: " + tojson(validateRes));
assert.eq(validateRes.warnings.length, 0, "validate should not warn: " + tojson(validateRes));

rst.stopSet();
})();
