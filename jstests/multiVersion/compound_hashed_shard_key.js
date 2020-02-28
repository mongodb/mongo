/**
 * Tests the behaviour of compound hashed shard key with different FCV versions.
 *
 * Compound hashed shard key creation is enabled in 4.4. In this multi version test, we ensure that
 *  - We cannot create compound hashed shard key on binary 4.4 in FCV 4.2 or when binary is 4.2.
 *  - We can create compound hashed shard key when FCV is 4.4.
 *  - We cannot downgrade FCV to 4.2 in the presence of a collection with compound hashed shard key.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");                // For assertStagesForExplainOfCommand.
load("jstests/multiVersion/libs/multi_cluster.js");  // For upgradeCluster.
load("jstests/replsets/rslib.js");                   // For awaitRSClientHosts.

TestData.skipCheckDBHashes = true;  // Skip db hashes when restarting the replset.

// When checking UUID consistency, the shell attempts to run a command on the node it believes is
// primary in each shard. However, this test restarts shards, and the node that is elected primary
// after the restart may be different from the original primary. Since the shell does not retry on
// NotMaster errors, and whether or not it detects the new primary before issuing the command is
// nondeterministic, skip the consistency check for this test.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

const nodeOptions42 = {
    binVersion: "last-stable"
};
const nodeOptions44 = {
    binVersion: "latest"
};
const kDbName = jsTestName();
const ns = kDbName + '.coll';

// Set up a new sharded cluster consisting of 3 nodes, initially running on 4.2 binaries.
const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 3},
    other: {
        mongosOptions: nodeOptions42,
        configOptions: nodeOptions42,
        rsOptions: nodeOptions42,
    }
});

let mongosDB = st.s.getDB(kDbName);
let coll = mongosDB.coll;
coll.drop();

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

/**
 * Upgrade the cluster to given version and refresh the connection variables.
 */
function upgradeCluster(version, components) {
    const defaultComponents = {upgradeMongos: false, upgradeShards: false, upgradeConfigs: false};
    components = Object.assign(defaultComponents, components);
    st.upgradeCluster(version, components);

    // Wait for the config server and shards to become available.
    st.configRS.awaitSecondaryNodes();
    st.rs0.awaitSecondaryNodes();
    st.rs1.awaitSecondaryNodes();

    // Wait for the ReplicaSetMonitor on mongoS and each shard to reflect the state of both shards.
    for (let client of [st.s, st.rs0.getPrimary(), st.rs1.getPrimary()]) {
        awaitRSClientHosts(
            client, [st.rs0.getPrimary(), st.rs1.getPrimary()], {ok: true, ismaster: true});
    }

    mongosDB = st.s.getDB(jsTestName());
    coll = mongosDB.coll;
}

// Restarts the given replset node.
function restartReplSetNode(replSet, node, options) {
    const defaultOpts = {remember: true, appendOptions: true, startClean: false};
    options = Object.assign(defaultOpts, (options || {}));

    // Merge the new options into the existing options for the given nodes.
    Object.assign(replSet.nodeOptions[`n${replSet.getNodeId(node)}`], options);
    replSet.restart([node], options);
}

// Verify that the cluster is on binary version 4.2 and FCV 4.2.
assertVersionAndFCV(["4.2"], lastStableFCV);

// Cannot create a compound hashed shard key on a cluster running binary 4.2.
assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, st.shard0.shardName);
assert.commandFailedWithCode(
    st.s.adminCommand({shardCollection: ns, key: {a: 1, b: "hashed", c: 1}}), ErrorCodes.BadValue);

// Upgrade the cluster to the new binary version, but keep the feature compatibility version at 4.2.
upgradeCluster(nodeOptions44.binVersion,
               {upgradeMongos: true, upgradeShards: true, upgradeConfigs: true});
assertVersionAndFCV(["4.4", "4.3"], lastStableFCV);

// Verify that the shard key cannot be refined to a compound hashed shard key.
assert.commandWorked(st.s.adminCommand({shardCollection: kDbName + ".refine_sk", key: {a: 1}}));
assert.commandFailedWithCode(
    st.s.adminCommand({refineCollectionShardKey: kDbName + ".refine_sk", key: {x: "hashed", y: 1}}),
    ErrorCodes.CommandNotSupported);

// Cannot create a compound hashed shard key on binary 4.4 with FCV 4.2.
assert.commandFailedWithCode(
    st.s.adminCommand({shardCollection: ns, key: {a: 1, b: "hashed", c: 1}}), ErrorCodes.BadValue);

// Can create a compound hashed shard key on binary 4.4 with FCV 4.4.
assert.commandWorked(mongosDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {a: 1, b: "hashed", c: 1}}));

// Cannot set FCV to 4.2 when there is an existing collection with compound hashed shard key.
assert.commandFailedWithCode(mongosDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}),
                             31411);

// Create a collection with a compound hashed index before attempting to downgrade FCV to 4.2. We
// will subsequently test that this cannot be used to shard a collection while in FCV 4.2.
const pre42DowngradeHashedIndexColl = mongosDB.pre42DowngradeHashedIndexColl;
assert.commandWorked(
    pre42DowngradeHashedIndexColl.insert([{_id: 0, a: 1, b: 1, c: 1}, {_id: 1, a: 2, b: 2}]));
assert.commandWorked(pre42DowngradeHashedIndexColl.createIndex({a: "hashed", b: 1, c: 1}));

// Can set FCV to 4.2 after dropping the collection.
coll.drop();
assert.commandWorked(mongosDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

// Verify that we cannot create compound hashed shard key after downgrading to FCV 4.2, even if
// there is an existing compound hashed index.
assert.commandFailedWithCode(
    st.s.adminCommand(
        {shardCollection: pre42DowngradeHashedIndexColl.getFullName(), key: {a: "hashed", b: 1}}),
    ErrorCodes.BadValue);
pre42DowngradeHashedIndexColl.drop();

// Set FCV back to latest and create collection with CHSK.
assert.commandWorked(mongosDB.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {a: 1, b: "hashed", c: 1}}));

// Set the fail point on config server to force FCV downgrade.
assert.commandWorked(st.configRS.getPrimary().getDB('admin').runCommand(
    {configureFailPoint: "allowFCVDowngradeWithCompoundHashedShardKey", mode: "alwaysOn"}));
assert.commandWorked(mongosDB.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));

// CRUD operations on an existing sharded collection with compound hashed shard key works even with
// FCV 4.2.
assert.commandWorked(coll.insert([
    {_id: 1, a: 2, b: 2},
    {_id: 2, b: 10, c: 2},
    {_id: 3, a: 1, b: 3, c: 1},
    {_id: 4, a: 1, b: 4, c: 1}
]));
assert.commandWorked(coll.update({c: 2}, {$set: {p: 1}}));
assert.commandWorked(coll.remove({b: 3}));
assert.sameMembers(coll.find({a: 2, b: 2}).toArray(), [{_id: 1, a: 2, b: 2}]);

// Verify that the sharding admin commands will also work.
let configDB = st.s.getDB('config');
let lowestChunk = configDB.chunks.find({ns: ns}).sort({min: 1}).limit(1).next();
assert(lowestChunk);
assert.commandWorked(st.s.adminCommand({split: ns, bounds: [lowestChunk.min, lowestChunk.max]}));

// Find the new lowest chunk and move.
lowestChunk = configDB.chunks.find({ns: ns}).sort({min: 1}).limit(1).next();
assert.commandWorked(st.s.adminCommand(
    {moveChunk: ns, bounds: [lowestChunk.min, lowestChunk.max], to: st.shard1.shardName}));

// Starting mongos with 4.2 binary does not fail but read/write operations cannot be performed on
// the collection. This is because mongos cannot understand compound hashed shard key while trying
// to target the operation to the respective shard(s).
upgradeCluster(nodeOptions42.binVersion, {upgradeMongos: true});
assert.commandFailedWithCode(coll.insert({a: 1, b: 1, c: 1}), ErrorCodes.BadValue);
assert.commandFailedWithCode(coll.update({_id: 0}, {$set: {p: 1}}), ErrorCodes.BadValue);
assert.commandFailedWithCode(coll.remove({_id: 0}), ErrorCodes.BadValue);
assert.commandFailedWithCode(coll.runCommand({find: coll.getName(), filter: {}}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(coll.runCommand({find: coll.getName(), filter: {}}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(coll.runCommand({find: coll.getName(), filter: {}}),
                             ErrorCodes.BadValue);

// Verify that sharding admin commands will also fails.
configDB = st.s.getDB('config');
lowestChunk = configDB.chunks.find({ns: ns}).sort({min: 1}).limit(1).next();
assert(lowestChunk);
assert.commandFailedWithCode(
    st.s.adminCommand({split: ns, bounds: [lowestChunk.min, lowestChunk.max]}),
    ErrorCodes.BadValue);
assert.commandFailedWithCode(
    st.s.adminCommand(
        {moveChunk: ns, bounds: [lowestChunk.min, lowestChunk.max], to: st.shard1.shardName}),
    ErrorCodes.BadValue);

// Verify that the shards cannot be downgraded to 4.2 binary in the presense of compound hashed
// index. This should force users to drop the collection.
const secondaryNodeOfShard = st.rs0.getSecondaries()[0];
assert(secondaryNodeOfShard);
try {
    restartReplSetNode(st.rs0, secondaryNodeOfShard, nodeOptions42);
    assert(false, "Expected 'restartCluster' to throw");
} catch (err) {
    assert.eq(err.message, `Failed to connect to node ${st.rs0.getNodeId(secondaryNodeOfShard)}`);
    // HashAccessMethod should throw this error when the index spec is validated during startup.
    assert(rawMongoProgramOutput().match("exception in initAndListen: Location16763"));
}
// Start that node and mongos with the 4.4 binary for a clean shutdown.
st.rs0.start(secondaryNodeOfShard, nodeOptions44);
st.rs0.awaitReplication();
upgradeCluster(nodeOptions44.binVersion, {upgradeMongos: true});

st.stop();
}());