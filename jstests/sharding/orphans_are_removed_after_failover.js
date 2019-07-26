/**
 * Ensure that orphaned documents are deleted after a migration or a failover.
 *
 * 1. Create a sharded collection with two chunks on Shard A
 * 2. Pause range deletion on the primary of Shard A
 * 3. Migrate a chunk from Shard A to Shard B
 * 4. Cause a step down on Shard A
 * 5. Connect directly to the new primary of Shard A and verify that eventually no documents remain
 *    from the chunk that was migrated away
 */

(function() {
"use strict";

load('./jstests/libs/cleanup_orphaned_util.js');

TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

// Create 2 shards with 3 replicas each.
let st = new ShardingTest({shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});

// Create a sharded collection with three chunks:
//     [-inf, -10), [-10, 10), [10, inf)
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: -10}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 10}}));

// Move chunk [10, inf) to shard1.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 10}, to: st.shard1.shardName}));

let testDB = st.s.getDB(dbName);
let testColl = testDB.foo;

// Insert 20 docs in first chunk.
for (var i = -100; i < -80; i++) {
    testColl.insert({x: i});
}

// Insert 10 docs in second chunk.
for (var i = -5; i < 5; i++) {
    testColl.insert({x: i});
}

// Insert 10 docs in third chunk.
for (var i = 15; i < 25; i++) {
    testColl.insert({x: i});
}

const expectedNumDocsTotal = 40;
const expectedNumDocsShard0Before = 30;
const expectedNumDocsShard1Before = 10;
const expectedNumDocsShard0After = 20;
const expectedNumDocsShard1After = 20;

// Verify total count.
assert.eq(testColl.find().itcount(), expectedNumDocsTotal);

// Verify shard0 count.
let shard0Coll = st.shard0.getCollection(ns);
assert.eq(shard0Coll.find().itcount(), expectedNumDocsShard0Before);

// Verify shard1 count.
let shard1Coll = st.shard1.getCollection(ns);
assert.eq(shard1Coll.find().itcount(), expectedNumDocsShard1Before);

// Pause range deletion.
let originalShard0Primary = st.rs0.getPrimary();
originalShard0Primary.adminCommand({configureFailPoint: 'suspendRangeDeletion', mode: 'alwaysOn'});

// Move chunk [-10, 10) to shard1.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: -10}, to: st.shard1.shardName}));

// Verify total count.
assert.eq(testColl.find().itcount(), expectedNumDocsTotal);

// Since the range deleter is paused, we expect the orphaned documents to still be on shard 0,
// so the document count should be the same as it was before the migration.
assert.eq(shard0Coll.find().itcount(), expectedNumDocsShard0Before);

// Verify shard1 count contains moved chunk.
assert.eq(shard1Coll.find().itcount(), expectedNumDocsShard1After);

// Step down current primary.
assert.commandWorked(st.rs0.getPrimary().adminCommand({replSetStepDown: 60, force: 1}));

// Allow the range deleter to run.
originalShard0Primary.adminCommand({configureFailPoint: 'suspendRangeDeletion', mode: 'off'});

// Connect to new primary for shard0.
let shard0Primary = st.rs0.getPrimary();
let shard0PrimaryColl = shard0Primary.getCollection(ns);

// Verify that orphans are not yet deleted on the new primary.
assert.eq(shard0PrimaryColl.find().itcount(), expectedNumDocsShard0Before);

// TODO SERVER-41800: Remove call to cleanupOrphaned from range deleter acceptance test.
const expectedNumIterations = 2;
cleanupOrphaned(shard0Primary, ns, expectedNumIterations);

// Verify that orphans are deleted.
assert.eq(shard0PrimaryColl.find().itcount(), expectedNumDocsShard0After);

st.stop();
})();