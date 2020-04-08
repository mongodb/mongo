/*
 * Utilities for dealing with chunk bounds.
 */
var chunkBoundsUtil = (function() {
    let _gte = function(shardKeyA, shardKeyB) {
        return bsonWoCompare(shardKeyA, shardKeyB) >= 0;
    };

    let _lt = function(shardKeyA, shardKeyB) {
        return bsonWoCompare(shardKeyA, shardKeyB) < 0;
    };

    let containsKey = function(shardKey, minKey, maxKey) {
        return _gte(shardKey, minKey) && _lt(shardKey, maxKey);
    };

    /*
     * Returns a object mapping each shard name to an array of chunk bounds
     * that it owns.
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
            if (st._connections[i].shardName == shardName) {
                return st._connections[i];
            }
        }
    };

    /*
     * Returns the shard object for the shard that owns the chunk that contains
     * the given shard key value and the bounds of the chunk.
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
     */
    let findShardForShardKey = function(st, shardChunkBounds, shardKey) {
        return findShardAndChunkBoundsForShardKey(st, shardChunkBounds, shardKey).shard;
    };

    return {
        containsKey,
        findShardChunkBounds,
        findShardAndChunkBoundsForShardKey,
        findShardForShardKey
    };
})();