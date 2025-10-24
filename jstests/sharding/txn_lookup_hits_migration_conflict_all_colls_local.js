/**
 * Tests that a concurrent migration will cause an aggregation running in a txn to fail even when
 * the routing shard can take advantage of the query optimization to skip routing over the network,
 * and instead push down to sbe (which occurs when the secondary sharded collection is fully local).
 *
 * @tags: [
 *   assumes_balancer_off,
 *   requires_sharding
 * ]
 */

const st = new ShardingTest({mongos: 1, shards: 2});

const dbName = 'test_txn_with_chunk_migration';
const collName1 = 'coll1';
const collName2 = 'coll2';
const ns1 = dbName + '.' + collName1;
const ns2 = dbName + '.' + collName2;

st.s.getDB(dbName).dropDatabase();

let coll1 = st.s.getDB(dbName)[collName1];
let coll2 = st.s.getDB(dbName)[collName2];

st.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName});
st.adminCommand({shardCollection: ns1, key: {a: 1}});
st.adminCommand({shardCollection: ns2, key: {x: 1}});
assert.commandWorked(st.splitAt(ns2, {x: 0}));
assert.commandWorked(st.moveChunk(ns2, {x: -1}, st.shard0.shardName));
assert.commandWorked(st.moveChunk(ns2, {x: 1}, st.shard1.shardName));

assert.commandWorked(coll1.insert({a: 1}));
assert.commandWorked(coll2.insert({x: -1}));
assert.commandWorked(coll2.insert({x: 1}));

{
    const session = st.s.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl1 = sessionDB.getCollection(collName1);

    session.startTransaction();

    // Opens snapshot at time T on both shards
    sessionColl1.find({a: 1}).itcount();

    // Migration at time T+1 - move chunk with x:1 from shard1 to shard0
    /*
     * Distribution at T:
     * shard0: ns2: {x: -1}, ns1: {a: 1}
     * shard1: ns2 {x: 1}
     * Distribution at T+1 after migration:
     * shard0: ns2: {x: -1, x: 1}, ns1: {a: 1}
     * shard1: {}
     */
    assert.commandWorked(st.moveChunk(ns2, {x: 1}, st.shard0.shardName));

    // A non-transactional agg should find: a:1 matches x:1
    const lookupPipeline =
        [{$lookup: {from: collName2, localField: "a", foreignField: "x", as: "result"}}];
    let firstResult =
        st.s.getDB(dbName).getCollection(collName1).aggregate(lookupPipeline).toArray();
    jsTest.log("First aggregation result: " + tojson(firstResult));
    assert.eq(1, firstResult[0].result.length, "First lookup should find match for a:1 with x:1");

    // Run the same agg in the open transaction, and assert it fails with MigrationConflict.
    assert.commandFailedWithCode(assert.throws(() => sessionColl1.aggregate(lookupPipeline)),
                                              ErrorCodes.MigrationConflict);

    // Cleanup
    for (let db of [st.shard0.getDB('config'), st.shard1.getDB('config')]) {
        assert.commandWorked(db.runCommand({killSessions: [session.id]}));
    }
}

st.stop();