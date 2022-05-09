/**
 * Tests that in 6.0 version listIndexes can parse invalid index specs created before 5.0 version.
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
rst.upgradeSet({binVersion: "last-lts"});
assert.commandWorked(rst.getPrimary().adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

// Upgrades from 5.0 to 6.0.
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

rst.stopSet();
})();