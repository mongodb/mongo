load("jstests/sharding/libs/chunk_bounds_util.js");
load("jstests/sharding/libs/find_chunks_util.js");

/**
 * Asserts that the given shards have the given chunks.
 *
 * @param shardChunkBounds {Object} a map from each shard name to an array of the bounds for all
 *                                  the chunks on the shard. Each pair of chunk bounds is an array
 *                                  of the form [minKey, maxKey].
 */
function assertChunksOnShards(configDB, ns, shardChunkBounds) {
    for (let [shardName, chunkBounds] of Object.entries(shardChunkBounds)) {
        for (let bounds of chunkBounds) {
            assert.eq(
                shardName,
                findChunksUtil.findOneChunkByNs(configDB, ns, {min: bounds[0], max: bounds[1]})
                    .shard,
                "expected to find chunk " + tojson(bounds) + " on shard \"" + shardName + "\"");
        }
    }
}

/**
 * Asserts that the docs are on the shards that own their corresponding chunks.
 *
 * @param shardChunkBounds {Object} a map from each shard name to an array of the bounds for all
 *                                  the chunks on the shard. Each pair of chunk bounds is an array
 *                                  of the form [minKey, maxKey].
 * @param shardKey         {Object} a map from each shard key field to 1 if the collection uses
 *                                  range based sharding and "hashed" if the collection uses
 *                                  hashed sharding. (i.e. equivalent to the value passed for the
 *                                  "key" field for the shardCollection command).
 */
function assertDocsOnShards(st, ns, shardChunkBounds, docs, shardKey) {
    for (let doc of docs) {
        let docShardKey = {};
        for (const [k, v] of Object.entries(shardKey)) {
            docShardKey[k] = (v == "hashed") ? convertShardKeyToHashed(doc[k]) : doc[k];
        }
        let shard = chunkBoundsUtil.findShardForShardKey(st, shardChunkBounds, docShardKey);
        assert.eq(1,
                  shard.getCollection(ns).count(doc),
                  "expected to find doc " + tojson(doc) + " on shard \"" + shard.shardName + "\"");
    }
}

/**
 * Asserts that the given shards have the given tags.
 *
 * @param shardTags {Object} a map from each shard name to an array of strings representing the zone
 *                           names that the shard owns.
 */
function assertShardTags(configDB, shardTags) {
    for (let [shardName, tags] of Object.entries(shardTags)) {
        assert.eq(tags.sort(),
                  configDB.shards.findOne({_id: shardName}).tags.sort(),
                  "expected shard \"" + shardName + "\" to have tags " + tojson(tags.sort()));
    }
}

/**
 * Adds toShard to zone and removes fromShard from zone.
 */
function moveZoneToShard(st, zoneName, fromShard, toShard) {
    assert.commandWorked(st.s.adminCommand({addShardToZone: toShard.shardName, zone: zoneName}));
    assert.commandWorked(
        st.s.adminCommand({removeShardFromZone: fromShard.shardName, zone: zoneName}));
}

/**
 * Starts the balancer, lets it run for at least the given number of rounds,
 * then stops the balancer.
 */
function runBalancer(st, minNumRounds) {
    st.startBalancer();

    // We add 1 to the number of rounds to avoid a race condition
    // where the first round is a no-op round
    for (let i = 0; i < minNumRounds + 1; ++i)
        st.awaitBalancerRound();

    st.stopBalancer();
}

/**
 * Updates the zone key range for the given namespace.
 */
function updateZoneKeyRange(st, ns, zoneName, fromRange, toRange) {
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: ns, min: fromRange[0], max: fromRange[1], zone: null}));
    assert.commandWorked(st.s.adminCommand(
        {updateZoneKeyRange: ns, min: toRange[0], max: toRange[1], zone: zoneName}));
}
