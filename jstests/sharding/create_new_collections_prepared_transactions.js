// Test that new collection creation succeeds in a sharded write transaction.
//
// @tags: [
//   requires_sharding,
//   uses_multi_shard_transaction,
//   uses_transactions,
// ]
import {
    retryOnceOnTransientAndRestartTxnOnMongos
} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbNameShard0 = "test";
const dbNameShard2 = "testOther";
const collName = "foo";

const st = new ShardingTest({
    shards: 3,
    mongos: 1,
    rs: {nodes: 2},
});

const versionSupportsSingleWriteShardCommitOptimization =
    MongoRunner.compareBinVersions(jsTestOptions().mongosBinVersion, "7.1") >= 0;

// Create two databases with different shards as their primaries.
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbNameShard0, primaryShard: st.shard0.shardName}));
assert.commandWorked(
    st.s.getDB(dbNameShard0)[collName].insert({_id: 5}, {writeConcern: {w: "majority"}}));

// Set up another collection with a different shard (shard2) as its primary shard.
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbNameShard2, primaryShard: st.shard2.shardName}));
assert.commandWorked(
    st.s.getDB(dbNameShard2)[collName].insert({_id: 4}, {writeConcern: {w: "majority"}}));

const session = st.s.getDB(dbNameShard0).getMongo().startSession({causalConsistency: false});

let sessionDBShard0 = session.getDatabase(dbNameShard0);
let sessionDBShard2 = session.getDatabase(dbNameShard2);
const newCollName = "newColl";

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

// TODO SERVER-77915: Remove when deleting the feature flag.
if (FeatureFlagUtil.isPresentAndEnabled(st.s, "CreateCollectionInPreparedTransactions")) {
    assert.commandWorked(session.commitTransaction_forTesting());

    assertDropCollection(st.s.getDB(dbNameShard0), newCollName);
    assertDropCollection(st.s.getDB(dbNameShard2), newCollName);
} else {
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.OperationNotSupportedInTransaction);
}

jsTest.log("Testing collection creation in a single-shard write transaction.");
session.startTransaction(txnOptions);
assert.commandWorked(sessionDBShard0.createCollection(newCollName));
doc2 = sessionDBShard2.getCollection(collName).findOne({_id: 4});
assert.eq(doc2._id, 4);
if (!versionSupportsSingleWriteShardCommitOptimization) {
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.OperationNotSupportedInTransaction);
} else {
    assert.commandWorked(session.commitTransaction_forTesting());
}

st.stop();
