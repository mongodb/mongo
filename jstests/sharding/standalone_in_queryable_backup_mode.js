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

import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test shuts down a shard.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckDBHashes = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;
TestData.skipCheckMetadataConsistency = true;

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

const shard0Identity = st.rs0.getPrimary().getDB("admin").getCollection("system.version").findOne({
    _id: "shardIdentity"
});

assert.commandWorked(
    st.s.adminCommand({shardCollection: kDbName + '.' + kShardedCollName, key: {_id: 1}}));

assert.commandWorked(st.s.getDB(kDbName).createCollection(kUnshardedCollName));

const recoveryTimestamp =
    assert.commandWorked(st.rs0.getPrimary().getDB(kDbName).runCommand({ping: 1})).operationTime;

jsTest.log("Going to hold the stable timestamp of the secondary node at " +
           tojson(recoveryTimestamp));
// Hold back the recovery timestamp before doing another write so we have some oplog entries to
// apply when restart in queryableBackupMode with recoverToOplogTimestamp.
assert.commandWorked(st.rs0.getSecondary().getDB('admin').adminCommand({
    "configureFailPoint": 'holdStableTimestampAtSpecificTimestamp',
    "mode": 'alwaysOn',
    "data": {"timestamp": recoveryTimestamp}
}));

jsTest.log("Going to apply some CRUD operations over sharded and unsharded collections");
function applyCRUDOnColl(coll) {
    coll.insert({age: 42});
    coll.update({age: 42}, {$set: {name: "john"}});
    coll.deleteMany({});
    // Leaving one document around in the collection is intentional: it validates that the fast
    // count stays accurate after replaying the oplog in queryable backup mode. See SERVER-88931.
    coll.insert({age: 43});
}
applyCRUDOnColl(st.s.getDB(kDbName)[kShardedCollName]);
applyCRUDOnColl(st.s.getDB(kDbName)[kUnshardedCollName]);
st.rs0.awaitReplication();

jsTest.log("Going to stop the secondary node of the shard");
const operationTime =
    assert.commandWorked(st.rs0.getPrimary().getDB(kDbName).runCommand({ping: 1})).operationTime;
const secondaryPort = st.rs0.getSecondary().port;
const secondaryDbPath = st.rs0.getSecondary().dbpath;
MongoRunner.stopMongod(st.rs0.getSecondary());

jsTest.log(
    "Going to start a mongod process with --shardsvr, --queryableBackupMode and recoverToOplogTimestamp");
let configFileStr =
    "sharding:\n _overrideShardIdentity: '" + tojson(shard0Identity).replace(/\s+/g, ' ') + "'";
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
assert.eq(newMongoD.getDB(kDbName)[kShardedCollName].find({}).itcount(), 1);
assert.eq(newMongoD.getDB(kDbName)[kUnshardedCollName].find({}).itcount(), 1);

jsTest.log("Going to stop the shardsvr queryable backup mode mongod process");
MongoRunner.stopMongod(newMongoD);

jsTest.log("Going to stop the sharding test");
st.stop();
})();
