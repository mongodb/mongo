/**
 * The goal of this test is to apply some oplog entries during the startup of a mongod process
 * configured with --shardsvr and --queryableBackupMode.
 *
 * @tags: [
 *   # In-memory storage engine does not support queryable backups.
 *   requires_persistence,
 *   # Config shards do not support queryable backups.
 *   config_shard_incompatible
 * ]
 */

(function() {
'use strict';

const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    rs: {nodes: 2},
});

jsTest.log("Going to set up the environment");
var kDbName = 'testDb';
var kShardedCollName = 'testShardedColl';
var kUnshardedCollName = 'testUnshardedColl';

assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
assert.commandWorked(
    st.s.adminCommand({shardCollection: kDbName + '.' + kShardedCollName, key: {_id: 1}}));

const recoveryTimestamp =
    assert.commandWorked(st.rs0.getPrimary().getDB(kDbName).runCommand({ping: 1})).operationTime;

jsTest.log("Going to hold the stable timestamp of the secondary node at " +
           tojson(recoveryTimestamp));
// Hold back the recovery timestamp before doing another write so we have some oplog entries to
// apply when restart in queryableBackupMode with recoverToOplogTimestamp.
const secondary = st.rs0.getSecondary();
assert.commandWorked(secondary.getDB('admin').adminCommand({
    "configureFailPoint": 'holdStableTimestampAtSpecificTimestamp',
    "mode": 'alwaysOn',
    "data": {"timestamp": recoveryTimestamp}
}));

jsTest.log("Going to apply some CRUD operations over sharded and unsharded collections");
function applyCRUDOnColl(coll) {
    coll.insert({age: 42});
    coll.update({age: 42}, {$set: {name: "john"}});
    coll.deleteMany({});
}
applyCRUDOnColl(st.s.getDB(kDbName)[kShardedCollName]);
applyCRUDOnColl(st.s.getDB(kDbName)[kUnshardedCollName]);
st.rs0.awaitReplication();

jsTest.log("Going to stop the secondary node of the shard");
const operationTime =
    assert.commandWorked(st.rs0.getPrimary().getDB(kDbName).runCommand({ping: 1})).operationTime;
const secondaryPort = secondary.port;
const secondaryDbPath = secondary.dbpath;
// Remove the secondary from the cluster since we will restart it in queryable backup mode later.
const secondaryId = st.rs0.getNodeId(secondary);
st.rs0.remove(secondaryId);
st.rs0.reInitiate();

jsTest.log(
    "Going to start a mongod process with --shardsvr, --queryableBackupMode and recoverToOplogTimestamp");
const shardIdentity = st.rs0.getPrimary().getDB("admin").getCollection("system.version").findOne({
    _id: "shardIdentity"
});
let configFileStr =
    "sharding:\n _overrideShardIdentity: '" + tojson(shardIdentity).replace(/\s+/g, ' ') + "'";
let delim = _isWindows() ? '\\' : '/';
let configFilePath = secondaryDbPath + delim + "config-for-read-only-mongod.yml";
writeFile(configFilePath, configFileStr);

const newMongoD = MongoRunner.runMongod({
    config: configFilePath,
    dbpath: secondaryDbPath,
    port: secondaryPort,
    noCleanData: true,
    setParameter: {recoverToOplogTimestamp: tojson({timestamp: operationTime})},
    queryableBackupMode: "",
    shardsvr: "",
});

jsTest.log("Going to verify the number of documents of both collections");
assert.eq(newMongoD.getDB(kDbName)[kShardedCollName].find({}).itcount(), 0);
assert.eq(newMongoD.getDB(kDbName)[kUnshardedCollName].find({}).itcount(), 0);

jsTest.log("Going to stop the shardsvr queryable backup mode mongod process");
MongoRunner.stopMongod(newMongoD);

jsTest.log("Going to stop the sharding test");
st.stop();
})();
