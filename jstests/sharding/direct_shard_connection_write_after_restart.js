/**
 * Test that write operations succeed when using a direct connection against a freshly restarted
 * shard. Direct connections to a shard are expected for a time period for users who are in the
 * process of migrating from a replica set to a one-shard cluster. The cluster may become sharded
 * before the application starts to connect to mongos rather than the shard.
 *
 * When the shard is restarted, its in-memory filtering metadata is unknown and needs to
 * be recovered from the config server. In this case, writes issued directly against the shard may
 * fail with a StaleConfig error. However, this causes mongod to refresh its filtering metadata. As
 * a result, the StaleConfig error is transient and the write may be retried by the client.
 *
 * This test was originally written to reproduce SERVER-95244.
 *
 * @tags: [
 *   # This test restarts a shard and expects its state to persist after restart.
 *   requires_persistence,
 * ]
 */

var st = new ShardingTest({shards: 1});

// Returns a handle to collection "test.foo" that uses a direct connection to the shard,
// circumventing mongos.
function getCollWithDirectShardConn() {
    let shardConn = st.rs0.getPrimary();
    return shardConn.getDB("test")["foo"];
}

// Create the collection first, because shards install sharding filtering metadata upon create.
assert.commandWorked(getCollWithDirectShardConn().insert({x: 0}));

const tests = [
    {
        desc: "upsert that results in an insert",
        doWrite: (directColl) =>
            directColl.update({x: 1}, {$inc: {y: 1}}, {upsert: true, multi: false}),
        expectedCollection: [{x: 0}, {x: 1, y: 1}]
    },
    {
        desc: "upsert that results in an insert, but with multi:true",
        doWrite: (directColl) =>
            directColl.update({x: 2}, {$inc: {y: 1}}, {upsert: true, multi: true}),
        expectedCollection: [{x: 0}, {x: 1, y: 1}, {x: 2, y: 1}]
    },
    {
        desc: "upsert that results in a regular update",
        doWrite: (directColl) =>
            directColl.update({x: 2}, {$inc: {y: 1}}, {upsert: true, multi: true}),
        expectedCollection: [{x: 0}, {x: 1, y: 1}, {x: 2, y: 2}]
    },
    {
        desc: "non-upsert multi:true update which affects multiple documents",
        doWrite: (directColl) =>
            directColl.update({x: {$gte: 1}}, {$inc: {y: 1}}, {upsert: false, multi: true}),
        expectedCollection: [{x: 0}, {x: 1, y: 2}, {x: 2, y: 3}]
    },
    {
        desc: "non-upsert multi:false update",
        doWrite: (directColl) =>
            directColl.update({x: 2}, {$inc: {y: 1}}, {upsert: false, multi: false}),
        expectedCollection: [{x: 0}, {x: 1, y: 2}, {x: 2, y: 4}]
    },
    {
        desc: "insert",
        doWrite: (directColl) => directColl.insert({x: 3}),
        expectedCollection: [{x: 0}, {x: 1, y: 2}, {x: 2, y: 4}, {x: 3}]
    },
    {
        desc: "delete one document",
        doWrite: (directColl) => directColl.remove({x: 3}, {justOne: true}),
        expectedCollection: [{x: 0}, {x: 1, y: 2}, {x: 2, y: 4}]
    },
    {
        desc: "delete multiple documents",
        doWrite: (directColl) => directColl.remove({x: {$gte: 1}}, {justOne: false}),
        expectedCollection: [{x: 0}]
    },
];

for (let testCase of tests) {
    jsTestLog("Starting test case: " + testCase.desc);

    // Restart the node. After the restart, the shard's sharding filtering metadata (in-memory) is
    // unknown and needs to be recovered from the config svr.
    st.restartShardRS(0);

    // Using a direct to shard connection, attempt to upsert a document that doesn't currently
    // exist.
    let directColl = getCollWithDirectShardConn();

    // The first write should fail with StaleConfig, but re-issuing the operation should allow it to
    // succeed.
    assert.commandFailedWithCode(testCase.doWrite(directColl), ErrorCodes.StaleConfig);
    assert.commandWorked(testCase.doWrite(directColl));

    // Run a query to verify that the state of the collection is as expected.
    assert.eq(directColl.find({}, {_id: 0}).sort({x: 1}).toArray(), testCase.expectedCollection);
}

st.stop();
