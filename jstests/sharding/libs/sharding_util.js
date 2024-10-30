/**
 * Helpers for creating and accessing sharding metadata.
 */

export function getShardNames(db) {
    return db.adminCommand({listShards: 1}).shards.map(shard => shard._id);
}

/**
 * Returns an array of chunks such that each shard has a chunk range that roughly equally covers
 * [min, max]
 */
export function createChunks(shardNames, shardKey, min, max) {
    let chunks = [];
    let rangeSize = (max - min) / 2;

    if ((max - min + 1) < shardNames.length) {
        throw new Error("[min, max] range is not large enough");
    }

    if (shardNames.length == 1) {
        return [{shard: shardNames[0], min: {[shardKey]: MinKey}, max: {[shardKey]: MaxKey}}];
    }

    for (let i = 0; i < shardNames.length; i++) {
        if (i == 0) {
            chunks.push({min: {[shardKey]: MinKey}, max: {[shardKey]: min}, shard: shardNames[i]});
        } else if (i == shardNames.length - 1) {
            chunks.push({
                min: {[shardKey]: min + (i - 1) * rangeSize},
                max: {[shardKey]: MaxKey},
                shard: shardNames[i]
            });
        } else {
            chunks.push({
                min: {[shardKey]: min + (i - 1) * rangeSize},
                max: {[shardKey]: min + i * (rangeSize)},
                shard: shardNames[i]
            });
        }
    }

    return chunks;
}
