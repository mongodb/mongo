/**
 * Test for SERVER-8786 - if the first operation on an authenticated shard is moveChunk, it breaks
 * the cluster.
 *
 * Any tests that restart a shard mongod and send sharding requests to it after restart cannot make
 * the shard use an in-memory storage engine, since the shardIdentity document will be lost after
 * restart.
 *
 * @tags: [requires_persistence]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

// The UUID consistency check uses connections to shards cached on the ShardingTest object, but this
// test restarts a shard, so the cached connection is not usable.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

let st = new ShardingTest({shards: 2, other: {keyFile: "jstests/libs/key1", useHostname: true, chunkSize: 1}});

let mongos = st.s;
let adminDB = mongos.getDB("admin");
var db = mongos.getDB("test");

adminDB.createUser({user: "admin", pwd: "password", roles: jsTest.adminUserRoles});

adminDB.auth("admin", "password");

adminDB.runCommand({enableSharding: "test", primaryShard: st.shard1.shardName});
adminDB.runCommand({shardCollection: "test.foo", key: {x: 1}});

for (let i = 0; i < 100; i++) {
    db.foo.insert({x: i});
}

adminDB.runCommand({split: "test.foo", middle: {x: 50}});
let curShard = st.getShard("test.foo", {x: 75});
let otherShard = st.getOther(curShard).name;
adminDB.runCommand({moveChunk: "test.foo", find: {x: 25}, to: otherShard, _waitForDelete: true});

st.printShardingStatus();

st.rs0.stopSet(undefined, true);
st.rs0.startSet({}, true);

// May fail the first couple times due to socket exceptions
assert.soon(function () {
    let res = adminDB.runCommand({moveChunk: "test.foo", find: {x: 75}, to: otherShard});
    printjson(res);
    return res.ok;
});

printjson(db.foo.findOne({x: 25}));
printjson(db.foo.findOne({x: 75}));

st.stop();
