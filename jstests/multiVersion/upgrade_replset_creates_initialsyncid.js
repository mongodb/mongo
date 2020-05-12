/**
 * Tests that upgrading a replset adds the 'local.replset.initialSyncId' document.
 */

(function() {
"use strict";

load('jstests/multiVersion/libs/multi_rs.js');
load("jstests/replsets/rslib.js");

const oldVersion = "last-stable";

const nodes = {
    n1: {binVersion: oldVersion},
    n2: {binVersion: oldVersion},
    n3: {binVersion: oldVersion, rsConfig: {priority: 0}},
};

const rst = new ReplSetTest({nodes: nodes});

rst.startSet();
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const localprimary = primary.getDB("local");
const localsecondary = secondary.getDB("local");
const collName = "replset.initialSyncId";

assert.eq(0, localprimary[collName].find().itcount());
assert.eq(0, localsecondary[collName].find().itcount());

jsTest.log("Upgrading replica set...");

rst.upgradeSet({binVersion: "latest"});

jsTest.log("Replica set upgraded.");

reconnect(primary);
reconnect(secondary);

const primarySyncIdData = localprimary[collName].find().toArray();
assert.eq(1, primarySyncIdData.length);
const primarySyncId = primarySyncIdData[0];
const secondarySyncIdData = localsecondary[collName].find().toArray();
assert.eq(1, secondarySyncIdData.length);
const secondarySyncId = secondarySyncIdData[0];

// We expect each node to pick a unique "_id".
assert.neq(0, bsonWoCompare(primarySyncId["_id"], secondarySyncId["_id"]));

rst.stopSet();
})();
