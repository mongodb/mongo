/**
 * Test that the availability canaries writing to the config database don't cause issues during various transitions:
 * to dedicated, to embedded, adding or removing shards.
 *
 * @tags: [
 * requires_fcv_83
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

jsTest.log.info("Create sharded cluster 'st' with embedded config server");
let st = new ShardingTest({
    name: "st",
    shards: 2,
    useHostname: false,
    other: {
        enableBalancer: true,
        configShard: true,
        rsOptions: {setParameter: {orphanCleanupDelaySecs: 0}},
    },
});

// Use retryWrites when writing to the configsvr because mongos does not automatically retry those.
const mongosSession = st.s.startSession({retryWrites: true});
const configDB = mongosSession.getDatabase("config");
const adminDB = mongosSession.getDatabase("admin");

// Verifies that config.availability doesn't get registered on the global catalog, that data is not moved between
// shards and the expected number of shards
const verify = (expectedNumberOfShards, dbsToCheck = []) => {
    const existingShards = configDB.shards.find({}).toArray();
    assert.eq(expectedNumberOfShards, existingShards.length, "Unexpected number of shards: " + tojson(existingShards));

    assert.eq(0, configDB.collections.find({_id: "config.availability"}).itcount());
    for (let [shardName, db] of dbsToCheck) {
        assert.eq([{_id: shardName}], db.availability.find().toArray());
    }
};

// Verify we have 2 shards initially, no data yet.
verify(2);

assert.commandWorked(st.shard1.getDB("config").availability.insert({_id: "shard1"}));
assert.commandWorked(st.configRS.getPrimary().getDB("config").availability.insert({_id: "config"}));

jsTest.log.info("Start transition to dedicated config server");
assert.commandWorked(adminDB.runCommand({startTransitionToDedicatedConfigServer: 1}));
st.configRS.awaitReplication();

jsTest.log.info("Wait for draining to complete");
assert.soon(() => {
    const drainingStatus = adminDB.runCommand({getTransitionToDedicatedConfigServerStatus: 1});
    assert.commandWorked(drainingStatus);
    return "drainingComplete" == drainingStatus.state;
}, "getTransitionToDedicatedConfigServerStatus did not return 'drainingComplete' status within the timeout");

jsTest.log.info("Commit transition to dedicated config server");
assert.commandWorked(adminDB.runCommand({commitTransitionToDedicatedConfigServer: 1}));

let dbsToCheck = [
    ["shard1", st.shard1.getDB("config")],
    ["config", st.configRS.getPrimary().getDB("config")],
];

// Verify after transition to dedicated, only 1 shard
verify(1, dbsToCheck);

// Create a shard and add to the cluster
const rs1 = new ReplSetTest({name: "addShard", host: "localhost", nodes: 1});
rs1.startSet({shardsvr: ""});
rs1.initiate();

const newShard = "myShard";
assert.commandWorked(rs1.getPrimary().getDB("config").availability.insert({_id: newShard}));
assert.eq(1, rs1.getPrimary().getDB("config").availability.find({_id: newShard}).itcount());

jsTest.log.info("Add new shard with existing data in config.availability");
assert.commandWorked(adminDB.runCommand({addShard: rs1.getURL(), name: newShard}));

const newShardDoc = configDB.shards.findOne({_id: newShard});
assert(newShardDoc.topologyTime instanceof Timestamp);

// Reverify after shard has been added
dbsToCheck.push([newShard, rs1.getPrimary().getDB("config")]);
verify(2, dbsToCheck);

// Remove older shard
assert.soon(() => {
    let removeResult = assert.commandWorked(adminDB.runCommand({removeShard: st.shard1.shardName}));
    return "completed" == removeResult.state;
}, "Failed to remove shard in time");

// Reverify after shard has been removed
dbsToCheck = dbsToCheck.filter(([name]) => name !== "shard1");
verify(1, dbsToCheck);

// Transition to embedded config server
assert.commandWorked(adminDB.runCommand({transitionFromDedicatedConfigServer: 1}));

// Reverify after transition to embedded
verify(2, dbsToCheck);

st.stop();
rs1.stopSet();
