(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");

function bulkInsert(coll, keyValue, sizeMBytes) {
    const big = 'X'.repeat(1024 * 1024);  // 1MB
    var bulk = coll.initializeUnorderedBulkOp();
    for (var i = 0; i < sizeMBytes; i++) {
        bulk.insert({x: keyValue, big: big});
    }
    assert.commandWorked(bulk.execute());
}

function assertNumJumboChunks(configDB, ns, expectedNumJumboChunks) {
    assert.eq(findChunksUtil.countChunksForNs(configDB, ns, {jumbo: true}), expectedNumJumboChunks);
}

function setGlobalChunkSize(st, chunkSizeMBytes) {
    // Set global chunk size
    assert.commandWorked(
        st.s.getDB("config").settings.update({_id: 'chunksize'},
                                             {$set: {value: chunkSizeMBytes}},
                                             {upsert: true, writeConcern: {w: 'majority'}}));
}

function setCollectionChunkSize(st, ns, chunkSizeMBytes) {
    assert.commandWorked(
        st.s.adminCommand({configureCollectionBalancing: ns, chunkSize: chunkSizeMBytes}));
}

// Test setup
var st = new ShardingTest({shards: 2, other: {chunkSize: 1}});

assert.commandWorked(
    st.s.adminCommand({enablesharding: "test", primaryShard: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: 'zoneShard0'}));

// Try to move unsuccessfully a 3MB chunk and check it gets marked as jumbo
{
    // Set the chunk range with a zone that will cause the chunk to be in the wrong place so the
    // balancer will be forced to attempt to move it out.
    assert.commandWorked(st.s.adminCommand({shardcollection: "test.foo", key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: 'test.foo', min: {x: 0}, max: {x: MaxKey}, zone: 'zoneShard0'}));

    var db = st.getDB("test");

    const big = 'X'.repeat(1024 * 1024);  // 1MB

    // Insert 3MB of documents to create a jumbo chunk, and use the same shard key in all of
    // them so that the chunk cannot be split.
    var bulk = db.foo.initializeUnorderedBulkOp();
    for (var i = 0; i < 3; i++) {
        bulk.insert({x: 0, big: big});
    }

    assert.commandWorked(bulk.execute());

    st.startBalancer();

    // Wait for the balancer to try to move the chunk and check it gets marked as jumbo.
    assert.soon(() => {
        let chunk = findChunksUtil.findOneChunkByNs(st.getDB('config'), 'test.foo', {min: {x: 0}});
        if (chunk == null) {
            // Balancer hasn't run and enforce the zone boundaries yet.
            return false;
        }

        assert.eq(st.shard1.shardName, chunk.shard, `${tojson(chunk)} was moved by the balancer`);
        return chunk.jumbo;
    });

    st.stopBalancer();
}

// Move successfully a 3MB chunk
// Collection chunkSize must prevail over global chunkSize setting
//     global chunkSize     -> 1MB
//     collection chunkSize -> 5MB
{
    const collName = "collA";
    const coll = st.s.getDB("test").getCollection(collName);
    const configDB = st.s.getDB("config");
    const splitPoint = 0;

    assert.commandWorked(st.s.adminCommand({shardcollection: coll.getFullName(), key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({
        updateZoneKeyRange: coll.getFullName(),
        min: {x: splitPoint},
        max: {x: MaxKey},
        zone: 'zoneShard0'
    }));

    bulkInsert(coll, splitPoint, 3);

    setCollectionChunkSize(st, coll.getFullName(), 5);
    setGlobalChunkSize(st, 1);

    // Move the 3MB chunk to shard0
    st.startBalancer();
    st.awaitCollectionBalance(coll);
    st.stopBalancer();

    const chunk =
        findChunksUtil.findOneChunkByNs(configDB, coll.getFullName(), {min: {x: splitPoint}});

    // Verify chunk has been moved to shard0
    assert.eq(st.shard0.shardName,
              chunk.shard,
              `${tojson(chunk)} was not moved to ${tojson(st.shard0.shardName)}`);
    assertNumJumboChunks(configDB, coll.getFullName(), 0);

    coll.drop();
}

// Try to move unsuccessfully a 3MB chunk and mark it as jumbo
// Collection chunkSize must prevail over global chunkSize setting
//     global chunkSize     -> 5MB
//     collection chunkSize -> 1MB
{
    const collName = "collB";
    const coll = st.s.getDB("test").getCollection(collName);
    const configDB = st.s.getDB("config");
    const splitPoint = 0;

    assert.commandWorked(st.s.adminCommand({shardcollection: coll.getFullName(), key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({
        updateZoneKeyRange: coll.getFullName(),
        min: {x: splitPoint},
        max: {x: MaxKey},
        zone: 'zoneShard0'
    }));

    bulkInsert(coll, splitPoint, 3);

    setCollectionChunkSize(st, coll.getFullName(), 1);
    setGlobalChunkSize(st, 5);

    // Try to move the 3MB chunk and mark it as jumbo
    st.startBalancer();

    assert.soon(() => {
        const chunk =
            findChunksUtil.findOneChunkByNs(configDB, coll.getFullName(), {min: {x: splitPoint}});
        if (chunk == null) {
            // Balancer hasn't run and enforce the zone boundaries yet.
            return false;
        }

        return chunk.jumbo;
    });

    st.stopBalancer();

    const chunk =
        findChunksUtil.findOneChunkByNs(configDB, coll.getFullName(), {min: {x: splitPoint}});

    // Verify chunk hasn't been moved to shard0 and it's jumbo
    assert.eq(st.shard1.shardName,
              chunk.shard,
              `${tojson(chunk)} was moved to ${tojson(st.shard0.shardName)}`);
    assertNumJumboChunks(configDB, coll.getFullName(), 1);

    coll.drop();
}

st.stop();
})();
