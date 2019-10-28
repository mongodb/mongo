/**
 * Tests the behaviour of compound hashed indexes with different FCV versions.
 *
 * Compound hashed index creation is enabled in 4.4. In this multi version test, we ensure that
 *  - We cannot create compound hashed index when binary is 4.4 and FCV is 4.2 or when binary
 *    is 4.2.
 *  - We can create compound hashed indexes when FCV is 4.4.
 *  - We can start the server with FCV 4.2 with existing compound hashed indexes.
 *  - We cannot start the server with 4.2 binary with existing compound hashed indexes.
 *  - A compound hashed index can be dropped while in FCV 4.2.
 *  - TODO SERVER-43913: Verify that a compound hashed index built in FCV 4.4 continues to work when
 *    we downgrade to FCV 4.2.
 */
(function() {
"use strict";

load("jstests/multiVersion/libs/multi_rs.js");  // For upgradeSet.
load("jstests/replsets/rslib.js");              // For startSetIfSupportsReadMajority.

TestData.skipCheckDBHashes = true;  // Skip db hashes when restarting the replset.

const nodeOptions42 = {
    binVersion: "last-stable"
};
const nodeOptions44 = {
    binVersion: "latest"
};

// Set up a new replSet consisting of 3 nodes, initially running on 4.2 binaries.
const rst = new ReplSetTest({nodes: 3, nodeOptions: nodeOptions42});

if (!startSetIfSupportsReadMajority(rst)) {
    jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
    rst.stopSet();
    return;
}
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

// Restarts the given replset nodes, or the entire replset if no nodes are specified.
function restartReplSetNodes(replSet, nodes, options) {
    const defaultOpts = {remember: true, appendOptions: true, startClean: false};
    options = Object.assign(defaultOpts, (options || {}));
    nodes = (nodes || replSet.nodes);
    assert(Array.isArray(nodes));
    for (let node of nodes) {
        // Merge the new options into the existing options for the given nodes.
        Object.assign(replSet.nodeOptions[`n${replSet.getNodeId(node)}`], options);
    }
    replSet.restart(nodes, options);
}

// Verify that the replset is on binary version 4.2 and FCV 4.2.
assertVersionAndFCV(testDB, ["4.2"], lastStableFCV);

jsTestLog("Cannot create a compound hashed index on a replset running binary 4.2.");
assert.commandFailedWithCode(coll.createIndex({a: "hashed", b: 1}), 16763);

// Upgrade the set to the new binary version, but keep the feature compatibility version at 4.2.
rst.upgradeSet(nodeOptions44);
testDB = rst.getPrimary().getDB(jsTestName());
coll = testDB.coll;
assertVersionAndFCV(testDB, ["4.4", "4.3"], lastStableFCV);

jsTestLog("Cannot create a compound hashed index on binary 4.4 with FCV 4.2.");
assert.commandFailedWithCode(coll.createIndex({c: 1, a: "hashed", b: 1}), 16763);
assert.commandFailedWithCode(coll.createIndex({a: "hashed", b: 1}), 16763);

jsTestLog("Can create a compound hashed index on binary 4.4 with FCV 4.4.");
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
assert.commandWorked(coll.createIndex({c: 1, a: "hashed", b: 1}));
assert.commandWorked(coll.createIndex({b: "hashed", c: 1}));
assert.commandWorked(coll.insert([{a: 1, b: 1, c: 1}, {a: 2, b: 2}]));
rst.awaitReplication();

jsTestLog("Can use an existing compound hashed index after downgrading FCV to 4.2.");
// TODO SERVER-43913: Verify that the queries can use the indexes created above.
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

jsTestLog("Cannot create a new compound hashed index after downgrading FCV to 4.2.");
let coll_other = testDB.coll;
assert.commandFailedWithCode(coll_other.createIndex({c: 1, a: "hashed", b: 1}), 16763);

jsTestLog("Can restart the replset in FCV 4.2 with a compound hashed index present.");
restartReplSetNodes(rst);
testDB = rst.getPrimary().getDB(jsTestName());
coll = testDB.coll;
assertVersionAndFCV(testDB, ["4.4", "4.3"], lastStableFCV);

jsTestLog(
    "Can restart the Secondaries in FCV 4.2 and resync the compound hashed index from the Primary.");
restartReplSetNodes(rst, rst.getSecondaries(), {startClean: true});
rst.awaitSecondaryNodes();
rst.awaitReplication();

// Verify that the Secondaries have both recreated the compound hashed index.
let secondaries = rst.getSecondaries();
assert.eq(secondaries.length, 2);
for (let sec of secondaries) {
    assert.eq(sec.getCollection(coll.getFullName())
                  .aggregate([{$indexStats: {}}, {$match: {"key.a": "hashed"}}])
                  .toArray()
                  .length,
              1);
}

jsTestLog("Can drop an existing compound hashed index in FCV 4.2.");
assert.commandWorked(coll.dropIndex({c: 1, a: "hashed", b: 1}));

jsTestLog("Cannot recreate the dropped compound hashed index in FCV 4.2.");
assert.commandFailedWithCode(coll.createIndex({c: 1, a: "hashed", b: 1}), 16763);

// Set the FCV to 4.2 and test that a 4.2 binary fails to start when a compound hashed index that
// was built on 4.4 is still present in the catalog.
jsTestLog("Cannot start 4.2 binary with compound hashed index present.");
assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
secondaries = rst.getSecondaries();
assert.eq(secondaries.length, 2);
rst.awaitReplication();
try {
    restartReplSetNodes(rst, [secondaries[0]], nodeOptions42);
    assert(false, "Expected 'restartReplSetNodes' to throw");
} catch (err) {
    assert.eq(err.message, `Failed to connect to node ${rst.getNodeId(secondaries[0])}`);
    // HashAccessMethod should throw this error when the index spec is validated during startup.
    assert(rawMongoProgramOutput().match("exception in initAndListen: Location16763"));
}

jsTestLog("Restart the failed node on binary 4.4 and gracefully shut down the replset.");
Object.assign(rst.nodeOptions[`n${rst.getNodeId(secondaries[0])}`], nodeOptions44);
rst.start(secondaries[0], nodeOptions44);

rst.awaitReplication();
rst.stopSet();
}());