
/**
 * High-level helper functions to support the interaction with the shards and routers of
 * core_sharding tests.
 */

function _getRandomElem(list) {
    return list[Math.floor(Math.random() * list.length)];
}

function _getShards(conn) {
    return assert.commandWorked(conn.adminCommand({listShards: 1})).shards;
}

export function getNumShards(conn) {
    return _getShards(conn).length;
}

export function getRandomShardId(conn, exclude = undefined) {
    let shards = _getShards(conn);

    if (exclude !== undefined) {
        // Normalize the exclusion parameter to a list.
        // This is to support callers that only pass a single shardId (type string) as second
        // argument
        let exclusionList = [].concat(exclude);

        let filteredShards = shards.filter(shard => !exclusionList.includes(shard['_id']));

        assert.gte(filteredShards.length,
                   1,
                   `Can't find a shard not in ${tojsononeline(exclusionList)}. All shards ${
                       tojsononeline(shards)}`);

        shards = filteredShards;
    }

    return _getRandomElem(shards)['_id'];
}
