const dbName = 'test';
const collName1 = 'foo';
const collName2 = 'bar';
const ns1 = dbName + '.' + collName1;
const ns2 = dbName + '.' + collName2;

const st = new ShardingTest({mongos: 1, shards: 2});

let coll1 = st.s.getDB(dbName)[collName1];
let coll2 = st.s.getDB(dbName)[collName2];

// Setup initial state:
//   ns1: unsharded collection on shard0, with documents: {a: 0}
//   ns2: sharded collection with chunks both on shard0 and shard1, with documents: {x: -1}, {x: 1}
st.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName});

st.adminCommand({shardCollection: ns2, key: {x: 1}});
assert.commandWorked(st.splitAt(ns2, {x: 0}));
assert.commandWorked(st.moveChunk(ns2, {x: -1}, st.shard0.shardName));
assert.commandWorked(st.moveChunk(ns2, {x: 0}, st.shard1.shardName));

assert.commandWorked(coll1.insert({a: 1}));

assert.commandWorked(coll2.insert({x: -1}));
assert.commandWorked(coll2.insert({x: 1}));

// Start a multi-document transaction and make one read on shard0
const session = st.s.startSession();
const sessionDB = session.getDatabase(dbName);
const sessionColl1 = sessionDB.getCollection(collName1);
const sessionColl2 = sessionDB.getCollection(collName2);
session.startTransaction();  // Default is local RC. With snapshot RC there's no bug.
assert.eq(1, sessionColl1.find().itcount());

// While the transaction is still open, move ns2's [0, 100) chunk to shard0.
assert.commandWorked(st.moveChunk(ns2, {x: 0}, st.shard0.shardName));
// Refresh the router so that it doesn't send a stale SV to the shard, which would cause the txn to
// be aborted.
assert.eq(2, coll2.find().itcount());

// Trying to read coll2 will result in an error. Note that this is not retryable even with
// enableStaleVersionAndSnapshotRetriesWithinTransactions enabled because the first statement
// aleady had an active snapshot open on the same shard this request is trying to contact.
let err = assert.throwsWithCode(() => {
    sessionColl2.find().itcount();
}, ErrorCodes.MigrationConflict);

assert.contains("TransientTransactionError", err.errorLabels, tojson(err));

st.stop();
