/**
 * Tests the behaviour of hidden indexes with 4.4 & 4.6 FCV and binary version combinations. In this
 * test we verify that:
 * - Index hiding is only possible in FCV 4.6 and that unhiding is possible in both FCV 4.6 and 4.4.
 * - A 4.4 mongos binary will start sucessfully when the index catalog has a hidden index.
 * - A 4.4 mongod binary will fail to start gracefully when the index catalog has a hidden index.
 * - A 4.4 mongod binary initial-syncing from a binary 4.6 / FCV 4.4 node will fail gracefully when
 *   encountering a hidden index.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");                // For assertStagesForExplainOfCommand.
load("jstests/multiVersion/libs/multi_cluster.js");  // For upgradeCluster.

TestData.skipCheckDBHashes = true;  // Skip db hashes when restarting the replset.

// When checking UUID consistency, the shell attempts to run a command on the node it believes is
// primary in each shard. However, this test restarts shards, and the node that is elected primary
// after the restart may be different from the original primary. Since the shell does not retry on
// NotMaster errors, and whether or not it detects the new primary before issuing the command is
// nondeterministic, skip the consistency check for this test.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

const nodeOptionsLastStable = {
    binVersion: "last-stable"
};
const nodeOptionsLatest = {
    binVersion: "latest"
};
const kDbName = jsTestName();

// Set up a new sharded cluster consisting of 3 nodes, initially running on last stable binaries.
const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 3},
    other: {
        mongosOptions: nodeOptionsLastStable,
        configOptions: nodeOptionsLastStable,
        rsOptions: nodeOptionsLastStable,
    }
});
let mongosDB = st.s.getDB(kDbName);
let coll = mongosDB.coll;
coll.drop();
const indexName = "testHiddenIndex";

// Verifies that the instance is running with the specified binary version and FCV.
function assertVersionAndFCV(versions, fcv) {
    const majorMinorVersion = mongosDB.version().substring(0, 3);
    assert(versions.includes(majorMinorVersion));
    assert.eq(assert
                  .commandWorked(st.rs0.getPrimary().adminCommand(
                      {getParameter: 1, featureCompatibilityVersion: 1}))
                  .featureCompatibilityVersion.version,
              fcv);
    assert.eq(assert
                  .commandWorked(st.rs1.getPrimary().adminCommand(
                      {getParameter: 1, featureCompatibilityVersion: 1}))
                  .featureCompatibilityVersion.version,
              fcv);
}

// Restarts the given replset node.
function restartReplSetNode(replSet, node, options) {
    const defaultOpts = {remember: true, appendOptions: true, startClean: false};
    options = Object.assign(defaultOpts, (options || {}));

    // Merge the new options into the existing options for the given nodes.
    Object.assign(replSet.nodeOptions[`n${replSet.getNodeId(node)}`], options);
    replSet.restart([node], options);
}

function getIndex(name) {
    return coll.getIndexes().filter(index => name === index.name)[0];
}

//
// Test the behaviour when both mongos and mongod are using 4.4 binary, FCV is set to 4.4.
//
(function() {
assertVersionAndFCV(["4.4", "4.3"], "4.4");
assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: kDbName + '.coll', key: {_id: 1}}));
mongosDB = st.s.getDB(kDbName);
coll = mongosDB.coll;
assert.commandWorked(coll.insert({p: 1, q: 2, r: 3}));

// Verify that usage of 'hidden' parameter fails.
assert.commandFailedWithCode(coll.createIndex({q: 1}, {hidden: true}),
                             ErrorCodes.InvalidIndexSpecificationOption);

assert.commandWorked(coll.createIndex({p: 1}, {name: indexName}));
assert.eq(getIndex(indexName).hidden, undefined);
assert.commandFailedWithCode(coll.hideIndex(indexName), ErrorCodes.InvalidOptions);
})();

//
// Test the behaviour when both mongos and mongod are using 4.6 binary, FCV is set to 4.6.
//
(function() {
// Upgrade the cluster to the new binary version.
st.upgradeCluster(
    nodeOptionsLatest.binVersion,
    {upgradeMongos: true, upgradeShards: true, upgradeConfigs: true, waitUntilStable: true});
mongosDB = st.s.getDB(kDbName);
coll = mongosDB.coll;
assert.commandWorked(mongosDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
assertVersionAndFCV(["4.6", "4.5"], latestFCV);

// Can hide indexes through createIndex on latest FCV.
const createAsHiddenIndexName = "createAsHidden";
assert.commandWorked(coll.createIndex({q: 1}, {hidden: true, name: createAsHiddenIndexName}));
assert.eq(getIndex(createAsHiddenIndexName).hidden, true);

// Can hide and unhide indexes through collMod on latest FCV.
assert.commandWorked(coll.createIndex({p: 1}, {name: indexName}));
assert.commandWorked(coll.hideIndex(indexName));
assert.eq(getIndex(indexName).hidden, true);
assert.commandWorked(coll.unhideIndex(indexName));
assert.eq(getIndex(indexName).hidden, undefined);

// Hide the index and set FCV to lastStable.
assert.commandWorked(coll.hideIndex(indexName));
})();

//
// Test the behaviour when both mongos and mongod are using 4.6 binary, FCV is set to 4.4.
//
(function() {
// Verify that we cannot hide the index but unhide existing after downgrading to last stable FCV.
assert.commandWorked(mongosDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
assert.commandFailedWithCode(coll.createIndex({r: 1}, {hidden: true}), 31449);

// Existing hidden index is not considered for plan.
assert.eq(getIndex(indexName).hidden, true);
assertStagesForExplainOfCommand(
    {coll: coll, cmdObj: {find: coll.getName(), filter: {p: 1}}, expectedStages: ["COLLSCAN"]});

// Verify that unhiding still works.
assert.commandWorked(coll.unhideIndex(indexName));
assert.eq(getIndex(indexName).hidden, undefined);
assertStagesForExplainOfCommand(
    {coll: coll, cmdObj: {find: coll.getName(), filter: {p: 1}}, expectedStages: ["IXSCAN"]});

// Cannot hide a visible index.
assert.commandFailedWithCode(coll.hideIndex(indexName), ErrorCodes.BadValue);
})();

//
// Test the behaviour when mongos is using 4.4 binary, mongod is using 4.6 binary and FCV is set
// to 4.4.
//
(function() {
// Set FCV to latest in order to hide an index and then switch back to last stable.
assert.commandWorked(mongosDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
assert.commandWorked(coll.hideIndex(indexName));
assert.commandWorked(mongosDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

// Starting mongos with 4.4 binary does not fail.
st.upgradeCluster(
    nodeOptionsLastStable.binVersion,
    {upgradeMongos: true, upgradeShards: false, upgradeConfigs: false, waitUntilStable: true});
coll = st.s.getDB(kDbName).coll;

// Existing hidden index is not considered for plan.
assertStagesForExplainOfCommand(
    {coll: coll, cmdObj: {find: coll.getName(), filter: {p: 1}}, expectedStages: ["COLLSCAN"]});

// Verify that the index can be made visible using 4.4 mongos.
assert.commandWorked(coll.unhideIndex(indexName));
assert.eq(getIndex(indexName).hidden, undefined);
assertStagesForExplainOfCommand(
    {coll: coll, cmdObj: {find: coll.getName(), filter: {p: 1}}, expectedStages: ["IXSCAN"]});

// Cannot hide a visible index.
assert.commandFailedWithCode(coll.hideIndex(indexName), ErrorCodes.BadValue);
})();

//
// Downgrading mongod to 4.4 binary after mongos is downgraded to 4.4 and FCV is set to 4.4.
//
(function() {
// Verify that the shards cannot be downgraded to 4.4 binary in the presence of hidden index.
const secondaryNodeOfShard = st.rs0.getSecondaries()[0];
assert(secondaryNodeOfShard);
try {
    restartReplSetNode(st.rs0, secondaryNodeOfShard, nodeOptionsLastStable);
    assert(false, "Expected 'restartReplSetNode' to throw");
} catch (err) {
    assert.eq(err.message, `Failed to connect to node ${st.rs0.getNodeId(secondaryNodeOfShard)}`);
    assert(rawMongoProgramOutput().match(
        "InvalidIndexSpecificationOption: The field 'hidden' is not valid for an index specification"));
}

// Verify that a clean restart on 4.4 binary in the presence of hidden index fails.
try {
    restartReplSetNode(
        st.rs0, secondaryNodeOfShard, Object.assign(nodeOptionsLatest, {startClean: true}));
    assert(false, "Expected 'restartReplSetNode' to throw");
} catch (err) {
    assert.eq(err.message, "MongoDB process stopped with exit code: 14");
    assert(rawMongoProgramOutput().match(
        "InvalidIndexSpecificationOption: The field 'hidden' is not valid for an index specification"));
}

// Start that node and mongos with the latest binary for a clean shutdown.
st.rs0.start(secondaryNodeOfShard, Object.assign(nodeOptionsLatest, {startClean: true}));
st.rs0.awaitReplication();
st.upgradeCluster(nodeOptionsLatest.binVersion,
                  {upgradeMongos: true, upgradeShards: false, upgradeConfigs: false});
})();

st.stop();
}());
