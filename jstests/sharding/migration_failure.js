//
// Tests that migration failures before and after commit correctly recover when possible.
//
// Also checks that the collection version on a source shard updates correctly after a migration.
//

import {ShardingTest} from "jstests/libs/shardingtest.js";

function waitAndGetShardVersion(conn, collNs) {
    let shardVersion = undefined;
    assert.soon(() => {
        shardVersion = conn.adminCommand({getShardVersion: collNs}).global;
        return !(typeof shardVersion == "string" && shardVersion == "UNKNOWN");
    });

    return shardVersion;
}

let st = new ShardingTest({shards: 2, mongos: 1});

let mongos = st.s0;
let admin = mongos.getDB("admin");
let coll = mongos.getCollection("foo.bar");

assert(admin.runCommand({enableSharding: coll.getDB() + "", primaryShard: st.shard0.shardName}).ok);
assert(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}).ok);
assert(admin.runCommand({split: coll + "", middle: {_id: 0}}).ok);

st.printShardingStatus();

jsTest.log("Testing failed migrations...");

let oldVersion = null;
let newVersion = null;

// failMigrationCommit -- this creates an error that aborts the migration before the commit
// migration command is sent.
assert.commandWorked(
    st.shard0.getDB("admin").runCommand({configureFailPoint: "failMigrationCommit", mode: "alwaysOn"}),
);

oldVersion = waitAndGetShardVersion(st.shard0, coll.toString());

assert.commandFailed(admin.runCommand({moveChunk: coll + "", find: {_id: 0}, to: st.shard1.shardName}));

newVersion = waitAndGetShardVersion(st.shard0, coll.toString());

assert.eq(oldVersion.t, newVersion.t, "The shard version major value should not change after a failed migration");
// Split does not cause a shard routing table refresh, but the moveChunk attempt will.
assert.eq(2, newVersion.i, "The shard routing table should refresh on a failed migration and show the split");

assert.commandWorked(st.shard0.getDB("admin").runCommand({configureFailPoint: "failMigrationCommit", mode: "off"}));

// migrationCommitNetworkError -- mimic migration commit command returning a network error,
// whereupon the config server is queried to determine that this commit was successful.
assert.commandWorked(
    st.shard0.getDB("admin").runCommand({configureFailPoint: "migrationCommitNetworkError", mode: "alwaysOn"}),
);

// Run a migration where there will still be chunks in the collection remaining on the shard
// afterwards. This will cause the collection's shardVersion to be bumped higher.
oldVersion = waitAndGetShardVersion(st.shard0, coll.toString());

assert.commandWorked(admin.runCommand({moveChunk: coll + "", find: {_id: 1}, to: st.shard1.shardName}));

newVersion = waitAndGetShardVersion(st.shard0, coll.toString());

assert.lt(oldVersion.t, newVersion.t, "The major value in the shard version should have increased");
assert.eq(1, newVersion.i, "The minor value in the shard version should be 1");

// Run a migration to move off the shard's last chunk in the collection. The collection's
// shardVersion will be reset.
oldVersion = waitAndGetShardVersion(st.shard0, coll.toString());

assert.commandWorked(admin.runCommand({moveChunk: coll + "", find: {_id: -1}, to: st.shard1.shardName}));

newVersion = waitAndGetShardVersion(st.shard0, coll.toString());

assert.gt(oldVersion.t, newVersion.t, "The version prior to the migration should be greater than the reset value");

assert.eq(0, newVersion.t, "The shard version should have reset, but the major value is not zero");
assert.eq(0, newVersion.i, "The shard version should have reset, but the minor value is not zero");

assert.commandWorked(
    st.shard0.getDB("admin").runCommand({configureFailPoint: "migrationCommitNetworkError", mode: "off"}),
);

st.stop();
