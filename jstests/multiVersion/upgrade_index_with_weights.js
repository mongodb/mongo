// Test to verify that upgrading a replica sets, which has an index with 'weights' parameter works
// correctly.
(function() {
"use strict";

load("jstests/multiVersion/libs/multi_rs.js");  // For upgradeSet.

TestData.skipCheckDBHashes = true;  // Skip db hashes when restarting the replset.

const nodeOptionsLastStable = {
    binVersion: "last-lts"
};
const nodeOptionsLatest = {
    binVersion: "latest"
};

// Set up a new replSet consisting of 2 nodes, initially running on 4.4 binaries.
const rst = new ReplSetTest({nodes: 2, nodeOptions: nodeOptionsLastStable});
rst.startSet();
rst.initiate();

let testDB = rst.getPrimary().getDB(jsTestName());
let coll = testDB.coll;
coll.drop();

// Verifies that the instance is running with the specified binary version and FCV.
function assertVersionAndFCV(db, versions, fcv) {
    const majorMinorVersion = db.version().substring(0, 3);
    assert(versions.includes(majorMinorVersion));
    assert.eq(
        assert.commandWorked(db.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}))
            .featureCompatibilityVersion.version,
        fcv);
}

// Verify that the replset is on binary version 4.4 and FCV 4.4.
assertVersionAndFCV(testDB, ["4.4"], lastLTSFCV);

assert.commandWorked(coll.createIndex({a: 1}, {name: "a_1", weights: {d: 1}}));
assert.commandWorked(coll.insert({a: 1}));
rst.awaitReplication();

// Upgrade the set to the latest binary version and FCV.
rst.upgradeSet(nodeOptionsLatest);
testDB = rst.getPrimary().getDB(jsTestName());
coll = testDB.coll;
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
assertVersionAndFCV(testDB, ["4.9", "5.0"], latestFCV);
assert.commandFailedWithCode(coll.createIndex({b: 1}, {weights: {d: 1}}),
                             ErrorCodes.CannotCreateIndex);

// Restart the secondary node clean, and verify that the index data is synced correctly.
const secondaryNode = rst.getSecondaries()[0];
const secondaryNodeOptions = rst.nodeOptions[`n${rst.getNodeId(secondaryNode)}`];
rst.restart(secondaryNode, Object.assign({startClean: true}, secondaryNodeOptions));
coll = rst.getPrimary().getDB(jsTestName()).coll;
const index = coll.getIndexes().filter(index => (index["name"] == "a_1"));
assert.eq(index.length, 1, index);
assert.eq(index[0]["weights"], {d: 1}, index);

rst.awaitReplication();
rst.stopSet();
}());
