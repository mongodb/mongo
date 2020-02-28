/**
 * Tests the behavior of cleanupOrphaned when the resumable range deleter is enabled. That is, tests
 * that cleanupOrphaned
 *
 * 1) Ignores the 'startingFromKey' parameter
 * 2) Waits for all possibly orphaned ranges to be deleted before returning
 * 3) Does not return 'stoppedAtKey', to preserve the former API of not returning 'stoppedAtKey'
 *    once all orphans have been deleted.
 *
 * requires_fcv_44 because it's explicitly testing the FCV 4.4 behavior of cleanupOrphaned.
 * @tags: [requires_fcv_44]
 */

(function() {

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

var st = new ShardingTest({shards: 2});

jsTest.log("Shard and split a collection");
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 1}}));

jsTest.log("Insert some documents");
const numDocs = 100;
var bulk = st.s.getCollection(ns).initializeUnorderedBulkOp();
for (var i = 0; i < numDocs; i++) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

jsTest.log("Assert that there are no range deletion tasks");
assert.eq(0, st.shard0.getDB("config").getCollection("rangeDeletions").count());
assert.eq(0, st.shard1.getDB("config").getCollection("rangeDeletions").count());

let suspendRangeDeletionShard0 = configureFailPoint(st.shard0, "suspendRangeDeletion");

jsTest.log("Move two chunks to create two orphaned ranges on the donor");
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 1}, to: st.shard1.shardName}));

jsTest.log("Since the recipient does not have orphaned ranges, cleanupOrphaned should return");
assert.eq(0, st.shard1.getDB("config").getCollection("rangeDeletions").count());
assert.commandWorked(st.shard1.adminCommand({
    cleanupOrphaned: ns,
    startingFromKey: {_id: 50} /* The startingFromKey parameter should be ignored */
}));

jsTest.log("Since the donor has two orphaned ranges, cleanupOrphaned should block");
assert.eq(2, st.shard0.getDB("config").getCollection("rangeDeletions").count());
assert.commandFailedWithCode(st.shard0.adminCommand({
    cleanupOrphaned: ns,
    startingFromKey: {_id: 50} /* The startingFromKey parameter should be ignored */,
    maxTimeMS: 10 * 1000
}),
                             ErrorCodes.MaxTimeMSExpired);
assert.eq(numDocs, st.shard0.getDB(dbName).getCollection(collName).count());

jsTest.log("Once the donor can cleans up the ranges, cleanupOrphaned should eventually return.");
suspendRangeDeletionShard0.off();
const res = st.shard0.adminCommand({
    cleanupOrphaned: ns,
    startingFromKey: {_id: 50} /* The startingFromKey parameter should be ignored */
});
assert.commandWorked(res);
assert.eq(0, st.shard0.getDB("config").getCollection("rangeDeletions").count());
assert.eq(0, st.shard0.getDB(dbName).getCollection(collName).count());

// The result should not have contained 'stoppedAtKey', since to match the API of the original
// cleanupOrphaned command, no 'stoppedAtKey' is returned once all orphans have been deleted.
assert.eq(null, res.stoppedAtKey);

st.stop();
})();
