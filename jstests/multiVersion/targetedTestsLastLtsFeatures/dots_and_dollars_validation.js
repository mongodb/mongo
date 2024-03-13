/**
 * Tests that dots and dollars validation in field names is enforced in 5.0 with 5.0 FCV
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

assert.commandFailed(primaryColl.insert({$fieldName: 1}));

jsTestLog("Upgrading to version 5.0");
rst.upgradeSet({binVersion: "5.0"});

const primary = rst.getPrimary();
primaryDB = primary.getDB(dbName);
primaryColl = primaryDB.getCollection(collName);

assert.commandFailed(primaryColl.insert({$fieldName: 1}));

assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "5.0"}));

assert.commandWorked(primaryColl.insert({$fieldName: 1}));

rst.stopSet();
})();
