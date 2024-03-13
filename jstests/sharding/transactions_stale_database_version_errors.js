// Tests mongos behavior on stale database version errors received in a transaction.
//
// @tags: [requires_sharding, uses_transactions, uses_multi_shard_transaction]
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    assertNoSuchTransactionOnAllShards,
    disableStaleVersionAndSnapshotRetriesWithinTransactions,
    enableStaleVersionAndSnapshotRetriesWithinTransactions,
} from "jstests/sharding/libs/sharded_transactions_helpers.js";

const dbName = "test";
const collName = "foo";

const st = new ShardingTest({shards: 2, mongos: 1});

// Database versioning tests only make sense when all collections are not tracked.
const isTrackUnshardedUponCreationEnabled = FeatureFlagUtil.isPresentAndEnabled(
    st.s.getDB('admin'), "TrackUnshardedCollectionsUponCreation");
if (isTrackUnshardedUponCreationEnabled) {
    st.stop();
    quit();
}

enableStaleVersionAndSnapshotRetriesWithinTransactions(st);

// Set up two unsharded collections in different databases with shard0 as their primary.
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: 0}, {writeConcern: {w: "majority"}}));

const session = st.s.startSession();
const sessionDB = session.getDatabase(dbName);

//
// Stale database version on first overall command should succeed.
//

session.startTransaction();

// No database versioned requests have been sent to Shard0, so it is stale.
assert.commandWorked(sessionDB.runCommand({distinct: collName, key: "_id", query: {_id: 0}}));

assert.commandWorked(session.commitTransaction_forTesting());

//
// Stale database version on second command to a shard should fail.
//
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

session.startTransaction();

// Run first statement on different database so distinct still triggers a SDV.
const dbName2 = "test2";
const sessionDB2 = session.getDatabase(dbName2);
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName2, primaryShard: st.shard1.shardName}));
assert.commandWorked(
    st.s.getDB(dbName2)[collName].insert({_id: 0}, {writeConcern: {w: "majority"}}));

assert.commandWorked(sessionDB2.runCommand({find: collName, filter: {_id: 0}}));

// Distinct is database versioned, so it will trigger SDV. The router will retry and the retry
// will discover the transaction was aborted, because a previous statement had completed on
// Shard0.
let res = assert.commandFailedWithCode(
    sessionDB.runCommand({distinct: collName, key: "_id", query: {_id: 0}}),
    ErrorCodes.NoSuchTransaction);
assert.eq(res.errorLabels, ["TransientTransactionError"]);

assertNoSuchTransactionOnAllShards(st, session.getSessionId(), session.getTxnNumber_forTesting());
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

//
// Stale database version on first command to a new shard should succeed.
//

// Create a new database on Shard0.
const otherDbName = "other_test";
const otherCollName = "bar";

assert.commandWorked(
    st.s.adminCommand({enableSharding: otherDbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(
    st.s.getDB(otherDbName)[otherCollName].insert({_id: 0}, {writeConcern: {w: "majority"}}));

const sessionOtherDB = session.getDatabase(otherDbName);

// Advance the router's cached last committed opTime for Shard0, so it chooses a read timestamp
// after the collection is created on shard1, to avoid SnapshotUnavailable.
assert.commandWorked(sessionOtherDB.runCommand({find: otherCollName}));  // Not database versioned.
assert.commandWorked(sessionDB[collName].insert({_id: 1}, {writeConcern: {w: "majority"}}));

session.startTransaction();

// Target the first database which is on Shard1.
assert.commandWorked(sessionDB.runCommand({distinct: collName, key: "_id", query: {_id: 0}}));

// Targets the new database on Shard0 which is stale, so a database versioned request should
// trigger SDV.
assert.commandWorked(
    sessionOtherDB.runCommand({distinct: otherCollName, key: "_id", query: {_id: 0}}));

assert.commandWorked(session.commitTransaction_forTesting());

//
// The final StaleDbVersion error should be returned if the router exhausts its retries.
//
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({movePrimary: otherDbName, to: st.shard1.shardName}));

// Disable database metadata refreshes on the stale shard so it will indefinitely return a stale
// version error.
assert.commandWorked(st.rs0.getPrimary().adminCommand(
    {configureFailPoint: "skipDatabaseVersionMetadataRefresh", mode: "alwaysOn"}));

session.startTransaction();

// Target Shard1, to verify the transaction on it is implicitly aborted later.
assert.commandWorked(sessionOtherDB.runCommand({find: otherCollName}));

// Target the first database which is on Shard0. The shard is stale and won't refresh its
// metadata, so mongos should exhaust its retries and implicitly abort the transaction.
res = assert.commandFailedWithCode(
    sessionDB.runCommand({distinct: collName, key: "_id", query: {_id: 0}}),
    ErrorCodes.StaleDbVersion);
assert.eq(res.errorLabels, ["TransientTransactionError"]);

// Verify all shards aborted the transaction.
assertNoSuchTransactionOnAllShards(st, session.getSessionId(), session.getTxnNumber_forTesting());
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

assert.commandWorked(st.rs0.getPrimary().adminCommand(
    {configureFailPoint: "skipDatabaseVersionMetadataRefresh", mode: "off"}));

disableStaleVersionAndSnapshotRetriesWithinTransactions(st);

st.stop();
