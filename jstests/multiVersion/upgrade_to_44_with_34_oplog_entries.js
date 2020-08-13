/**
 * Tests that upgrading to 4.4 succeeds when there are still oplog entries present that were
 * generated in 3.4, which lack the 'wall' field.
 */
(function() {
"use strict";

load("jstests/multiVersion/libs/multi_rs.js");

const dbName = "test";

const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: "3.4"}});
rst.startSet();
rst.initiate();

const bulk = rst.getPrimary().getDB(dbName).getCollection(jsTestName()).initializeUnorderedBulkOp();
for (let i = 0; i < 10000; i++) {
    bulk.insert({_id: i});
}
bulk.execute();

rst.upgradeSet({binVersion: "3.6"});
assert.commandWorked(
    rst.getPrimary().getDB(dbName).adminCommand({setFeatureCompatibilityVersion: "3.6"}));

rst.upgradeSet({binVersion: "4.0"});
assert.commandWorked(
    rst.getPrimary().getDB(dbName).adminCommand({setFeatureCompatibilityVersion: "4.0"}));

rst.upgradeSet({binVersion: "4.2"});
assert.commandWorked(
    rst.getPrimary().getDB(dbName).adminCommand({setFeatureCompatibilityVersion: "4.2"}));

rst.upgradeSet({binVersion: "latest"});
assert.commandWorked(
    rst.getPrimary().getDB(dbName).adminCommand({setFeatureCompatibilityVersion: latestFCV}));

rst.stopSet();
})();