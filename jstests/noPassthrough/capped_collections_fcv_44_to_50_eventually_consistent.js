/**
 * In FCV 4.4, each node is responsible for deleting the excess documents in capped collections.
 * This implies that capped deletes may not be synchronized between nodes at times. When upgraded to
 * FCV 5.0, the primary will generate delete oplog entries for capped collections. However, if any
 * secondary was behind in deleting excess documents while in FCV 4.4, the primary would have no way
 * of knowing and it would delete the first document it sees locally. Eventually, when secondaries
 * step up and start deleting capped documents, they will first delete previously missed documents
 * that may already be deleted on other nodes.
 *
 * This tests that secondaries with capped collections upgrading from FCV 4.4 to FCV 5.0 delete all
 * documents over the cap after stepping up.
 *
 * @tags: [
 *     requires_replication,
 *     # ephemeralForTest runs with 'oplogApplicationEnforcesSteadyStateConstraints=false' by
 *     # default.
 *     incompatible_with_eft,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

function assertContainsDocs(coll, docs) {
    for (const doc of docs) {
        assert.eq(1, coll.find(doc).itcount());
    }
}

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {setParameter: {"oplogApplicationEnforcesSteadyStateConstraints": true}}
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "4.4"}));

const dbName = "test";
const db = primary.getDB(dbName);
const secDB = secondary.getDB(dbName);

const collName = "eventually_consistent";
assert.commandWorked(
    db.createCollection(collName, {capped: true, size: 1024 * 1024 * 1024, max: 5}));

const coll = db.getCollection(collName);
const secColl = secDB.getCollection(collName);

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({x: i}));
}

// Pause capped deletes on the secondary.
let fp = configureFailPoint(secondary, "skipCappedDeletes");

for (let i = 5; i < 7; i++) {
    assert.commandWorked(coll.insert({x: i}));
}

rst.awaitReplication();

// Primary contains {x: 2} -> {x: 6}.
assert.eq(5, coll.find({}).itcount());
assertContainsDocs(coll, [{x: 2}, {x: 3}, {x: 4}, {x: 5}, {x: 6}]);

// Secondary contains {x: 0} -> {x: 6}.
assert.eq(7, secColl.find({}).itcount());
assertContainsDocs(secColl, [{x: 0}, {x: 1}, {x: 2}, {x: 3}, {x: 4}, {x: 5}, {x: 6}]);

assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "5.0"}));
fp.off();

assert.commandWorked(coll.insert({x: 7}));
rst.awaitReplication();

// Primary contains {x: 3} -> {x: 7}.
assert.eq(5, coll.find({}).itcount());
assertContainsDocs(coll, [{x: 3}, {x: 4}, {x: 5}, {x: 6}, {x: 7}]);

// Secondary contains {x: 0} -> {x: 1}, {x: 3} -> {x: 7}.
assert.eq(7, secColl.find({}).itcount());
assertContainsDocs(secColl, [{x: 0}, {x: 1}, {x: 3}, {x: 4}, {x: 5}, {x: 6}, {x: 7}]);

rst.stepUp(secondary);
rst.waitForState(secondary, ReplSetTest.State.PRIMARY);

assert.commandWorked(secColl.insert({x: 8}));
rst.awaitReplication();

// Deleting already deleted documents on the old primary is a no-op.
checkLog.containsJson(primary, 2170002);

// Old primary, now secondary contains {x: 4} -> {x: 8}.
assert.eq(5, coll.find({}).itcount());
assertContainsDocs(coll, [{x: 4}, {x: 5}, {x: 6}, {x: 7}, {x: 8}]);

// Old secondary, now primary contains {x: 4} -> {x: 8}.
assert.eq(5, secColl.find({}).itcount());
assertContainsDocs(secColl, [{x: 4}, {x: 5}, {x: 6}, {x: 7}, {x: 8}]);

rst.stopSet();
}());
