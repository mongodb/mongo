/**
 * Tests that WriteConflictException is handled when applying transfer mods during migrations.
 * @tags: [requires_fcv_44]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

let st = new ShardingTest({shards: 2});

// Create a sharded collection with two chunks: [-inf, 50), [50, inf)
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 50}}));

let testDB = st.s.getDB(dbName);
let testColl = testDB.foo;

for (let i = 0; i < 100; i++) {
    assert.commandWorked(testColl.insert({x: i}));
}

let preTransferModsFailpoint = configureFailPoint(st.shard1, "migrateThreadHangAtStep3");

const awaitResult = startParallelShell(
    funWithArgs(function(ns, toShardName) {
        assert.commandWorked(
            db.adminCommand({moveChunk: ns, find: {x: 50}, to: toShardName, _waitForDelete: true}));
    }, ns, st.shard1.shardName), st.s.port);

preTransferModsFailpoint.wait();

// Perform each operation that will generate a transfer mod operation in the migration thread. The
// migration thread processes inserts, updates and deletions which can all throw
// WriteConflictException.
for (let i = 100; i < 200; i++) {
    assert.commandWorked(testColl.insert({x: i}));
}

for (let i = 50; i < 75; ++i) {
    assert.commandWorked(testColl.remove({x: i}));
}

for (let i = 75; i < 100; ++i) {
    assert.commandWorked(testColl.update({x: i}, {x: i, updated: true}));
}

// Trigger WriteConflictExceptions during writes.
assert.commandWorked(st.shard1.adminCommand(
    {configureFailPoint: 'WTWriteConflictException', mode: {activationProbability: 0.5}}));
preTransferModsFailpoint.off();

awaitResult();

st.stop();
})();