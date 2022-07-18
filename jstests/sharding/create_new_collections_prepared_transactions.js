// Test that new collection creation fails in a cross-shard write transaction, but succeeds in a
// single-shard write transaction.
//
// @tags: [
//   requires_sharding,
//   uses_multi_shard_transaction,
//   uses_transactions,
// ]
(function() {
"use strict";

load("jstests/libs/auto_retry_transaction_in_sharding.js");

const dbNameShard0 = "test";
const dbNameShard2 = "testOther";
const collName = "foo";

const st = new ShardingTest({
    shards: 3,
    mongos: 1,
});

// Create two databases with different shards as their primaries.

assert.commandWorked(
    st.s.getDB(dbNameShard0)[collName].insert({_id: 5}, {writeConcern: {w: "majority"}}));

assert.commandWorked(st.s.adminCommand({enableSharding: dbNameShard0}));
st.ensurePrimaryShard(dbNameShard0, st.shard0.shardName);

// Set up another collection with a different shard (shard2) as its primary shard.
assert.commandWorked(
    st.s.getDB(dbNameShard2)[collName].insert({_id: 4}, {writeConcern: {w: "majority"}}));
st.ensurePrimaryShard(dbNameShard2, st.shard2.shardName);

const session = st.s.getDB(dbNameShard0).getMongo().startSession({causalConsistency: false});

let sessionDBShard0 = session.getDatabase(dbNameShard0);
let sessionDBShard2 = session.getDatabase(dbNameShard2);
let newCollName = "newColl";

// Ensure no stale version errors occur.
let doc = st.s.getDB(dbNameShard0).getCollection(collName).findOne({_id: 5});
assert.eq(doc._id, 5);
let doc2 = st.s.getDB(dbNameShard2).getCollection(collName).findOne({_id: 4});
assert.eq(doc2._id, 4);

jsTest.log("Testing collection creation in a cross-shard write transaction.");
const txnOptions = {
    writeConcern: {w: "majority"}
};
session.startTransaction(txnOptions);
retryOnceOnTransientAndRestartTxnOnMongos(session, () => {
    assert.commandWorked(sessionDBShard0.createCollection(newCollName));
    assert.commandWorked(sessionDBShard2.createCollection(newCollName));
}, txnOptions);
assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                             ErrorCodes.OperationNotSupportedInTransaction);

jsTest.log("Testing collection creation in a single-shard write transaction.");
// TODO (SERVER-48340): Re-enable the single-write-shard transaction commit optimization.
session.startTransaction(txnOptions);
assert.commandWorked(sessionDBShard0.createCollection(newCollName));
doc2 = sessionDBShard2.getCollection(collName).findOne({_id: 4});
assert.eq(doc2._id, 4);
assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                             ErrorCodes.OperationNotSupportedInTransaction);

st.stop();
})();
