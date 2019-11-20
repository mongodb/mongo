/*
 * Utilities for dealing with chunk bounds.
 */
var chunkBoundsUtil = (function() {
    let eq = function(shardKeyA, shardKeyB) {
        return bsonWoCompare(shardKeyA, shardKeyB) == 0;
    };

    let gte = function(shardKeyA, shardKeyB) {
        return bsonWoCompare(shardKeyA, shardKeyB) >= 0;
    };

    let lt = function(shardKeyA, shardKeyB) {
        return bsonWoCompare(shardKeyA, shardKeyB) < 0;
    };

    let max = function(shardKeyA, shardKeyB) {
        return gte(shardKeyA, shardKeyB) ? shardKeyA : shardKeyB;
    };

    let min = function(shardKeyA, shardKeyB) {
        return lt(shardKeyA, shardKeyB) ? shardKeyA : shardKeyB;
    };

    let containsKey = function(shardKey, minKey, maxKey) {
        return gte(shardKey, minKey) && lt(shardKey, maxKey);
    };

    let overlapsWith = function(chunkBoundsA, chunkBoundsB) {
        return containsKey(chunkBoundsA[0], chunkBoundsB[0], chunkBoundsB[1]) ||
            containsKey(chunkBoundsA[1], chunkBoundsB[0], chunkBoundsB[1]);
    };

    /*
     * Combines chunk bounds chunkBoundsA and chunkBoundsB. Assumes that the bounds
     * overlap.
     */
    let combine = function(chunkBoundsA, chunkBoundsB) {
        let rangeMin = min(chunkBoundsA[0], chunkBoundsB[0]);
        let rangeMax = max(chunkBoundsA[1], chunkBoundsB[1]);
        return [rangeMin, rangeMax];
    };

    /*
     * Computes the range that the given chunk bounds are in by combining the given chunk
     * bounds into bounds for one chunk. Assumes the chunk bounds are contiguous and in
     * nondescending order.
     */
    let computeRange = function(allChunkBounds) {
        let combinedBounds = allChunkBounds[0];
        for (let i = 1; i < allChunkBounds.length; i++) {
            assert(overlapsWith(combinedBounds, allChunkBounds[i]));
            combinedBounds = combine(combinedBounds, allChunkBounds[i]);
        }
        return combinedBounds;
    };

    /*
     * Returns a object mapping each shard name to an array of chunk bounds
     * that it owns.
     *
     * @param chunkDocs {Array} an array of chunk documents in the config database.
     */
    let findShardChunkBounds = function(chunkDocs) {
        let allBounds = {};
        for (let chunkDoc of chunkDocs) {
            let bounds = [chunkDoc.min, chunkDoc.max];

            if (!(chunkDoc.shard in allBounds)) {
                allBounds[chunkDoc.shard] = [bounds];
            } else {
                allBounds[chunkDoc.shard].push(bounds);
            }
        }
        return allBounds;
    };

    /*
     * Returns the corresponding shard object for the given shard name.
     */
    let _getShard = function(st, shardName) {
        for (let i = 0; i < st._connections.length; i++) {
            if (st["rs" + i].name == shardName) {
                return st._connections[i];
            }
        }
    };

    /*
     * Returns the shard object for the shard that owns the chunk that contains
     * the given shard key value and the bounds of the chunk.
     *
     * @param shardChunkBounds {Object} a map from each shard name to an array of the bounds
     *                                  for all the chunks on the shard. Each pair of chunk
     *                                  bounds is an array of the form [minKey, maxKey].
     */
    let findShardAndChunkBoundsForShardKey = function(st, shardChunkBounds, shardKey) {
        for (const [shardName, chunkBounds] of Object.entries(shardChunkBounds)) {
            for (let bounds of chunkBounds) {
                if (containsKey(shardKey, bounds[0], bounds[1])) {
                    return {shard: _getShard(st, shardName), bounds: bounds};
                }
            }
        }
    };

    /*
     * Returns the shard object for the shard that owns the chunk that contains
     * the given shard key value.
     *
     * @param shardChunkBounds {Object} a map from each shard name to an array of the bounds
     *                                  for all the chunks on the shard. Each pair of chunk
     *                                  bounds is an array of the form [minKey, maxKey].
     */
    let findShardForShardKey = function(st, shardChunkBounds, shardKey) {
        return findShardAndChunkBoundsForShardKey(st, shardChunkBounds, shardKey).shard;
    };

    return {
        eq,
        gte,
        lt,
        computeRange,
        containsKey,
        findShardChunkBounds,
        findShardAndChunkBoundsForShardKey,
        findShardForShardKey
    };
})();
