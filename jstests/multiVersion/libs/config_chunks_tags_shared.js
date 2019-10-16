/**
 * Functions and variables shared between multiversion/config_chunks_tags_upgrade_cluster.js and
 * multiversion/config_chunks_tags_downgrade_cluster.js.
 */

// Sets up a collection with chunks in the format expected by the testChunkOperations() helper.
function setUpCollectionForChunksTesting(st, ns) {
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: -50}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 1}, to: st.shard1.shardName}));
}

// Sets up zones and chunks for a collection to work with the testZoneOperations() helper.
function setUpCollectionForZoneTesting(st, ns) {
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "zone0"}));
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: "zone1"}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: ns, min: {_id: MinKey}, max: {_id: 0}, zone: "zone0"}));
    assert.commandWorked(
        st.s.adminCommand({updateZoneKeyRange: ns, min: {_id: 0}, max: {_id: 50}, zone: "zone1"}));
}

// Sets up a sharded collection with the given number of chunks and zones.
function setUpCollectionWithManyChunksAndZones(st, ns, numChunks, numZones) {
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    for (let i = 0; i < numChunks - 1; i++) {
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: i}}));
    }

    for (let i = 0; i < numZones; i++) {
        assert.commandWorked(
            st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "many_zones-" + i}));
        assert.commandWorked(st.s.adminCommand(
            {updateZoneKeyRange: ns, min: {_id: i}, max: {_id: i + 1}, zone: "many_zones-" + i}));
    }
}

function setUpExtraShardedCollections(st, dbName) {
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard1.shardName);

    // Set up one zone with half the key range and two chunks split at {_id: 0}.
    const ns = dbName + ".extra_coll";
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(
        st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: "extra_zone0"}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: ns, min: {_id: MinKey}, max: {_id: 0}, zone: "extra_zone0"}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard0.shardName}));

    // Set up a sharded collection with a hashed shard key.
    const hashedNs = dbName + ".extra_coll_hashed";
    assert.commandWorked(st.s.adminCommand({shardCollection: hashedNs, key: {_id: "hashed"}}));
}

function verifyChunksAndTags(st, dbName, chunkNs, zoneNs, {expectNewFormat}) {
    verifyChunks(st, {expectNewFormat});
    verifyTags(st, {expectNewFormat});

    testChunkOperations(st, chunkNs);
    testZoneOperations(st, zoneNs);
    verifyInitialChunks(st, dbName, {expectNewFormat});

    verifyChunks(st, {expectNewFormat});
    verifyTags(st, {expectNewFormat});
}

function getChunks(st, ns) {
    if (ns) {
        return st.s.getDB("config").chunks.find({ns}).sort({min: 1}).toArray();
    }
    return st.s.getDB("config").chunks.find().sort({min: 1}).toArray();
}

// Asserts all chunk documents have the expected format.
function verifyChunks(st, {ns, expectNewFormat}) {
    const chunks = getChunks(st, ns);
    assert.lte(1, chunks.length, tojson(chunks));
    chunks.forEach((chunk) => {
        if (expectNewFormat) {
            assert(chunk._id.isObjectId, tojson(chunk));
            assert.neq("string", typeof chunk._id, tojson(chunk));
        } else {
            assert(!chunk._id.isObjectId, tojson(chunk));
            assert.eq("string", typeof chunk._id, tojson(chunk));
        }

        let expectedChunkFields =
            ["_id", "ns", "min", "max", "shard", "lastmod", "lastmodEpoch", "history"];

        // Jumbo is an optional field.
        if (chunk.hasOwnProperty("jumbo")) {
            expectedChunkFields = expectedChunkFields.concat("jumbo");
        }

        assert.eq(Object.keys(chunk).length, expectedChunkFields.length, tojson(chunk));
        assert.hasFields(chunk, expectedChunkFields);
    });
}

function getTags(st) {
    return st.s.getDB("config").tags.find().sort({min: 1}).toArray();
}

// Asserts all tag documents have the expected format.
function verifyTags(st, {expectNewFormat}) {
    const tags = getTags(st);
    assert.lt(1, tags.length, tojson(tags));
    tags.forEach((tag) => {
        if (expectNewFormat) {
            assert(tag._id.isObjectId, tojson(tag));
            // ObjectId returns "object" from typeof...
            // assert.neq("object", typeof tag._id, tojson(tag));
        } else {
            assert(!tag._id.isObjectId, tojson(tag));
            assert.eq("object", typeof tag._id, tojson(tag));
        }

        const expectedTagFields = ["_id", "ns", "tag", "min", "max"];
        assert.eq(Object.keys(tag).length, expectedTagFields.length, tojson(tag));
        assert.hasFields(tag, expectedTagFields);
    });
}

// Runs basic crud operations against the given namespace.
function testCRUDOperations(st, ns) {
    const coll = st.s.getCollection(ns);
    assert.eq(0, coll.find().itcount());

    assert.commandWorked(coll.insert({_id: -5}));
    assert.commandWorked(coll.insert({_id: 5}));

    assert.commandWorked(coll.update({_id: -5}, {$set: {updated: true}}));
    assert.commandWorked(coll.update({_id: 5}, {$set: {updated: true}}));

    assert.docEq({_id: -5, updated: true}, coll.findOne({_id: -5}));
    assert.docEq({_id: 5, updated: true}, coll.findOne({_id: 5}));

    assert.commandWorked(coll.remove({_id: -5}, true /* justOne */));
    assert.commandWorked(coll.remove({_id: 5}, true /* justOne */));
    assert.eq(0, coll.find().itcount());
}

// Helper to verify chunks are owned by the expected shards.
function verifyChunkDistribution(st, ns, expectedChunkDistribution) {
    for (let i = 0; i < expectedChunkDistribution.length; i++) {
        assert.eq(expectedChunkDistribution[i],
                  st.s.getDB("config").chunks.count({ns: ns, shard: st["shard" + i].shardName}),
                  "unexpected number of chunks on shard " + i);
    }
}

// Assumes ns has the following chunk layout: [-inf, -50), [-50, 0) on shard0 and [0, inf) on
// shard 1.
function testChunkOperations(st, ns) {
    verifyChunkDistribution(st, ns, [2, 1]);

    // Split chunk should work.
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 50}}));
    verifyChunkDistribution(st, ns, [2, 2]);

    testCRUDOperations(st, ns);

    // Move chunk should work with a control chunk.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard0.shardName}));
    verifyChunkDistribution(st, ns, [3, 1]);

    testCRUDOperations(st, ns);

    // Move chunk should work without a control chunk.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 50}, to: st.shard0.shardName}));
    verifyChunkDistribution(st, ns, [4, 0]);

    testCRUDOperations(st, ns);

    // Merge chunk should work.
    assert.commandWorked(st.s.adminCommand({mergeChunks: ns, bounds: [{_id: -50}, {_id: 50}]}));
    verifyChunkDistribution(st, ns, [3, 0]);

    testCRUDOperations(st, ns);

    // Reset the chunks to their original state.
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    verifyChunkDistribution(st, ns, [4, 0]);
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 50}, to: st.shard1.shardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));
    verifyChunkDistribution(st, ns, [2, 2]);
    assert.commandWorked(st.s.adminCommand({mergeChunks: ns, bounds: [{_id: 0}, {_id: MaxKey}]}));
    verifyChunkDistribution(st, ns, [2, 1]);

    testCRUDOperations(st, ns);
}

// Assumes ns has two chunks: [-inf, 0), [0, inf), on shards 0 and 1, respectively and that shard0
// is in zone0 which contains [-inf, 0) and shard1 is in zone1 which contains [0, 50).
function testZoneOperations(st, ns) {
    // Verify conflicting zones can't be created.
    assert.commandFailedWithCode(
        st.s.adminCommand({updateZoneKeyRange: ns, min: {_id: -10}, max: {_id: 0}, zone: "zone1"}),
        ErrorCodes.RangeOverlapConflict);

    // Verify zone boundaries are still enforced.
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: ns, find: {_id: -1}, to: st.shard1.shardName}),
        ErrorCodes.IllegalOperation);

    //
    // Verify zone ranges can be updated.
    //

    assert.commandWorked(
        st.s.adminCommand({updateZoneKeyRange: ns, min: {_id: MinKey}, max: {_id: 0}, zone: null}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: ns, min: {_id: MinKey}, max: {_id: 0}, zone: "zone1"}));

    // Now the chunk can be moved to shard1, which is in zone1.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));

    // Reset the chunk and zones.
    assert.commandWorked(
        st.s.adminCommand({updateZoneKeyRange: ns, min: {_id: MinKey}, max: {_id: 0}, zone: null}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: ns, min: {_id: MinKey}, max: {_id: 0}, zone: "zone0"}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard0.shardName}));
}

let uniqueCollCounter = 0;

// Assumes ns is a non-existent namespace on a database that is sharding enabled.
function verifyInitialChunks(st, dbName, {expectNewFormat}) {
    const ns = dbName + ".unique_coll" + uniqueCollCounter++;
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    // Assert the chunks created for the new namespace are in the correct format.
    verifyChunks(st, {ns, expectNewFormat});

    // Clean up the new collection.
    assert.commandWorked(st.s.adminCommand({drop: ns}));
}
