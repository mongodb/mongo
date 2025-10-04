/**
 * Tests config shard topology.
 *
 * @tags: [
 *   requires_fcv_80,
 *   multiversion_incompatible,
 * ]
 */
import {moveChunkParallel} from "jstests/libs/chunk_manipulation_util.js";
import {ShardTransitionUtil} from "jstests/libs/shard_transition_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {moveDatabaseAndUnshardedColls} from "jstests/sharding/libs/move_database_and_unsharded_coll_helper.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

let staticMongod = MongoRunner.runMongod({});

const st = new ShardingTest({
    shards: {rs0: {nodes: 2}, rs1: {nodes: 2}},
    config: 2,
    mongos: 1,
    configShard: true,
});

assert.commandWorked(st.s0.adminCommand({enableSharding: "test", primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s0.getDB("test").user.insert({_id: 1234}));

assert.commandWorked(st.s0.adminCommand({enableSharding: "sharded", primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s0.adminCommand({shardCollection: "sharded.user", key: {_id: 1}}));

let configDbEntry = st.s.getDB("config").databases.findOne({_id: "test"});
let dbVersion = configDbEntry.version;

let shardedUserConfigColl = st.s.getDB("config").collections.findOne({_id: "sharded.user"});
let shardedUserConfigChunk = st.s.getDB("config").chunks.findOne({uuid: shardedUserConfigColl.uuid});
let shardVersion = {
    e: shardedUserConfigColl.lastmodEpoch,
    t: shardedUserConfigColl.timestamp,
    v: shardedUserConfigChunk.lastmod,
};

assert.commandWorked(st.s0.getDB("sharded").user.insert({_id: 5678}));

// Prime up config shard secondary's catalog cache.
let s0Conn = new Mongo(st.s0.host);
s0Conn.setReadPref("secondary");
let doc = s0Conn.getDB("test").user.findOne({_id: 1234});
assert.eq({_id: 1234}, doc);

doc = s0Conn.getDB("sharded").user.findOne({_id: 5678});
assert.eq({_id: 5678}, doc);

let removeRes = assert.commandWorked(st.s0.adminCommand({transitionToDedicatedConfigServer: 1}));
assert.eq("started", removeRes.state, tojson(removeRes));

assert.commandWorked(
    st.s0.adminCommand({moveChunk: "config.system.sessions", find: {_id: 0}, to: st.shard1.shardName}),
);

let joinMoveChunk = moveChunkParallel(
    staticMongod,
    st.s0.host,
    {_id: 0},
    null,
    "sharded.user",
    st.shard1.shardName,
    true /**Parallel should expect success */,
);

joinMoveChunk();

moveDatabaseAndUnshardedColls(st.s0.getDB("test"), st.shard1.shardName);
moveDatabaseAndUnshardedColls(st.s0.getDB("sharded"), st.shard1.shardName);

// A config shard can't be removed until all range deletions have finished.
ShardTransitionUtil.waitForRangeDeletions(st.s0);

removeRes = assert.commandWorked(st.s0.adminCommand({transitionToDedicatedConfigServer: 1}));
assert.eq("completed", removeRes.state, tojson(removeRes));

const downgradeFCV = binVersionToFCV("last-lts");
assert.commandWorked(st.s0.adminCommand({setFeatureCompatibilityVersion: downgradeFCV, confirm: true}));

// Connect directly to the config to simulate a stale mongos that thinks config server is
// still a shard

let configConn = new Mongo(st.configRS.getSecondary().host);
configConn.setReadPref("secondary");
configConn.setSlaveOk(true);

let findCmd = {
    find: "user",
    filter: {_id: 9876},
    databaseVersion: dbVersion,
    shardVersion: ShardVersioningUtil.kUntrackedShardVersion,
    readConcern: {level: "local"},
};
assert.commandFailedWithCode(configConn.getDB("test").runCommand(findCmd), ErrorCodes.StaleDbVersion);

// Note: secondary metadata gets cleared when replicating recoverable critical section.
let version = assert.commandWorked(configConn.adminCommand({getShardVersion: "sharded.user", fullMetadata: true}));
assert.eq({}, version.metadata);

findCmd = {
    find: "user",
    filter: {_id: 54321},
    shardVersion: shardVersion,
    readConcern: {level: "local"},
};
assert.commandFailedWithCode(configConn.getDB("sharded").runCommand(findCmd), ErrorCodes.StaleConfig);

version = assert.commandWorked(configConn.adminCommand({getShardVersion: "sharded.user", fullMetadata: true}));
assert.eq(1, timestampCmp(version.metadata.collVersion, shardVersion.v), tojson(version));
assert.eq(0, timestampCmp(version.metadata.shardVersion, Timestamp(0, 0)), tojson(version));

// Should be able to do secondary reads on the config server after transitioning back.

const upgradeFCV = binVersionToFCV("latest");
assert.commandWorked(st.s0.adminCommand({setFeatureCompatibilityVersion: upgradeFCV, confirm: true}));

// Need to drop the database before it can become a shard again.
assert.commandWorked(st.configRS.getPrimary().getDB("sharded").dropDatabase());

assert.commandWorked(st.s0.adminCommand({transitionFromDedicatedConfigServer: 1}));
moveDatabaseAndUnshardedColls(st.s0.getDB("test"), st.shard0.shardName);
assert.commandWorked(st.s0.adminCommand({moveChunk: "sharded.user", find: {_id: 0}, to: st.shard0.shardName}));

doc = s0Conn.getDB("test").user.findOne({_id: 1234});
assert.eq({_id: 1234}, doc);

doc = s0Conn.getDB("sharded").user.findOne({_id: 5678});
assert.eq({_id: 5678}, doc);

st.stop();

MongoRunner.stopMongod(staticMongod);
