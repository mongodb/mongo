/**
 * SERVER-110328 reproducer + regression guard.
 *
 * Scenario modelled in src/mongo/tla_plus/Sharding/ShardRegistryHostRotation:
 *
 *   Since SERVER-91121 the ShardRegistry refreshes from the configsvr only when it observes that
 *   `topologyTime' has advanced. A replSetReconfig that rotates the `hosts' field of a shard's
 *   `config.shards' entry (SERVER-21185) does NOT advance `topologyTime'. The cluster relies on the
 *   ReplicaSetMonitor (RSM) side channel to push the new connection string into the ShardRegistry.
 *
 *   If a router-node is partitioned away from every member it currently knows AND every host of the
 *   shard is then replaced, the RSM has no surviving reachable member to learn from, no
 *   topologyTime bump fires, and the ShardRegistry never refreshes - the node cannot talk to the
 *   shard until restart.
 *
 * This test exercises the happy path that the production fix must preserve: after a full host
 * rotation, the ShardRegistry on a mongos must eventually catch up to the new connection string
 * and route operations successfully. The test is deliberately conservative and does NOT simulate
 * the network-partition tail of the bug (which would deadlock without the production fix); that
 * tail is covered by the TLA+ bait invariant `BaitDivergenceStuck' in the spec.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Two shard replica sets, three nodes each. We rotate every host of rs1 and verify that the mongos
// ShardRegistry learns the new connection string without a restart.
const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 3},
    mongos: 1,
});

const dbName = "shardRegistryHostRotationDB";
const collName = "coll";
const ns = dbName + "." + collName;

const mongos = st.s0;
const adminDB = mongos.getDB("admin");
const testColl = mongos.getDB(dbName).getCollection(collName);

assert.commandWorked(adminDB.runCommand({enableSharding: dbName, primaryShard: st.shard1.shardName}));
assert.commandWorked(adminDB.runCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(testColl.insert({_id: 0, payload: "before-rotation"}));

const shardsColl = mongos.getCollection("config.shards");

function getShardDoc(rsName) {
    const doc = shardsColl.findOne({_id: rsName});
    assert.neq(null, doc, "config.shards entry missing for " + rsName);
    return doc;
}

function hostListSize(rsName) {
    return getShardDoc(rsName).host.split("/")[1].split(",").length;
}

const targetRs = st.rs1;
const rsName = targetRs.name;
const initialDoc = getShardDoc(rsName);
const initialHosts = initialDoc.host;
const initialTopologyTime = initialDoc.topologyTime;

jsTest.log("SERVER-110328: starting full host rotation on " + rsName +
           " (initial hosts=" + initialHosts + ")");

// Drive a full rotation by repeatedly adding a fresh node and removing the oldest one until none of
// the originally-known hosts remain. This mirrors the operational `replace every node` runbook the
// bug ticket describes.
const originalNodes = targetRs.nodes.slice();
const replConfigForNewNode = {rsConfig: {votes: 1, priority: 1}, shardsvr: ""};

for (let i = 0; i < originalNodes.length; i++) {
    const newNode = targetRs.add(replConfigForNewNode);
    targetRs.reInitiate();
    targetRs.awaitSecondaryNodes();

    const stepDownTarget = originalNodes[i];
    if (targetRs.getPrimary() === stepDownTarget) {
        assert.commandWorked(
            stepDownTarget.adminCommand({replSetStepDown: 60, secondaryCatchUpPeriodSecs: 30}));
        targetRs.awaitNodesAgreeOnPrimary();
    }
    targetRs.remove(stepDownTarget);
    const rsConfig = targetRs.getReplSetConfigFromNode();
    rsConfig.version++;
    assert.commandWorked(targetRs.getPrimary().adminCommand({replSetReconfig: rsConfig, force: true}));
    targetRs.awaitNodesAgreeOnPrimary();

    // Wait for the config.shards entry to reflect the new host count.
    assert.soon(
        () => hostListSize(rsName) === targetRs.nodes.length,
        () => "config.shards hosts not updated after rotation step " + i +
              ": " + tojson(getShardDoc(rsName)),
        5 * 60 * 1000);

    jsTest.log("SERVER-110328: rotation step " + i + " complete, hosts=" +
               getShardDoc(rsName).host);
}

const postRotationDoc = getShardDoc(rsName);
jsTest.log("SERVER-110328: post-rotation config.shards doc: " + tojson(postRotationDoc));

// Sanity: the host-set actually rotated; no original host remains.
const originalHostSet = new Set(initialHosts.split("/")[1].split(","));
const finalHostSet = new Set(postRotationDoc.host.split("/")[1].split(","));
for (const h of finalHostSet) {
    assert(!originalHostSet.has(h),
           "expected full host rotation; surviving host " + h + " is present in both sets");
}

// Sanity: topologyTime did NOT advance across the rotation (this is the SERVER-110328 precondition).
assert.eq(initialTopologyTime,
          postRotationDoc.topologyTime,
          "topologyTime unexpectedly advanced across host rotation; bug precondition no longer holds");

// The actual regression check: route a write through the same mongos without restarting it. With
// the fix in place, the ShardRegistry on this mongos must learn the new connection string via
// the configsvr-pull path that the fix introduces. Without the fix, this write hangs until the
// operation timeout because the ShardRegistry is pinned to the rotated-out host-set.
assert.commandWorked(testColl.insert({_id: 1, payload: "after-rotation"}),
                     "mongos failed to route post-rotation write; ShardRegistry did not refresh");

assert.eq(2, testColl.countDocuments({}), "post-rotation read disagrees with write history");

st.stop();
