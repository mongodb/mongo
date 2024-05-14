/**
 * Tests that a read-only transaction will choose a recovery shard if an additional participant is
 * ever marked as 'outstanding'.
 * @tags: [requires_fcv_80, uses_transactions]
 */

let st = new ShardingTest({mongos: 2, shards: 2});

const dbName = "test";
const localColl = "foo";
const foreignColl = "bar";
const localNs = dbName + "." + localColl;
const foreignNs = dbName + "." + foreignColl;

let shard0 = st.shard0;
let shard1 = st.shard1;

assert.commandWorked(st.s0.adminCommand({enableSharding: dbName, primaryShard: shard0.shardName}));

// Create a sharded collection, "foo", with one chunk on shard0.
assert.commandWorked(st.s0.adminCommand({shardCollection: localNs, key: {_id: 1}}));

// Create a sharded collection, "bar", with one chunk on shard0, and one chunk on shard1.
assert.commandWorked(st.s0.adminCommand({shardCollection: foreignNs, key: {x: 1}}));
assert.commandWorked(st.s0.adminCommand({split: foreignNs, middle: {x: 0}}));
assert.commandWorked(
    st.s0.adminCommand({moveChunk: foreignNs, find: {x: 0}, to: shard1.shardName}));

assert.commandWorked(st.s0.getDB(dbName).foo.insert([{_id: -5}]));
assert.commandWorked(st.s0.getDB(dbName).bar.insert([{_id: 1, x: -5}]));

// Run a transaction through the first mongos where shard0 will add itself as an additional
// participant and be picked the recovery shard.
const session = st.s0.startSession();
const sessionDB = session.getDatabase(dbName);

session.startTransaction();

// First, run a find that will target shard0 so that shard0 will be marked readOnly: true.
assert.eq(sessionDB.foo.find().toArray().length, 1);

// Now, run an aggregation where shard0 will target itself. The batchSize is not specified, so will
// be set to 0, so shard0 will respond to mongos and include itself in the additional participants
// list, but will not yet have a readOnly value because it will not wait to hear back from iteslf.
// This should cause shard0 to be marked as having an 'outstanding' readOnly value, and be marked as
// the recovery shard (because it will have to be treated as a write shard).
let res = sessionDB.runCommand({
    aggregate: localColl,
    pipeline: [
        {$lookup: {
            from: foreignColl,
            let: {localVar: "$_id"},
            pipeline: [
                {$match : {"_id": {$gte: -5}}},
            ], 
                as: "result"}},
        {$sort: {"_id": 1}}
    ],
    cursor: {}
});
assert.eq(res.cursor.firstBatch.length, 1);
assert.eq(res.cursor.firstBatch[0].result.length, 1);
let recoveryToken = res.recoveryToken;

// Commit the transaction on the mongos that has been routing the transaction requests so far
assert.commandWorked(session.commitTransaction_forTesting());

// Now recover the commit decision through the other mongos
const lsid = session.getSessionId();
assert.commandWorked(st.s1.getDB("admin").runCommand({
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: session.getTxnNumber_forTesting(),
    autocommit: false,
    recoveryToken: recoveryToken
}));

st.stop();
