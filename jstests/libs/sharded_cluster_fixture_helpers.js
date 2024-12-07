
/**
 * High-level helper functions to support the interaction with the shards and routers of
 * core_sharding tests.
 */

function _getRandomElem(list) {
    return list[Math.floor(Math.random() * list.length)];
}

function _getShardDescriptors(conn) {
    return assert.commandWorked(conn.adminCommand({listShards: 1})).shards;
}

export function getShardNames(conn) {
    return _getShardDescriptors(conn).map(shard => shard._id);
}

export function getNumShards(conn) {
    return _getShardDescriptors(conn).length;
}

export function getRandomShardName(conn, exclude = []) {
    // Normalize the exclusion parameter to a list, in case a single shardId is passed as a string.
    if (!Array.isArray(exclude)) {
        exclude = [exclude];
    }

    let shards = getShardNames(conn);
    let filteredShards = shards.filter(shard => !exclude.includes(shard));

    assert.gte(
        filteredShards.length,
        1,
        `Can't find a shard not in ${tojsononeline(exclude)}. All shards ${tojsononeline(shards)}`);

    return _getRandomElem(filteredShards);
}

// Drops and recreates dbName and assigns optPrimaryShard to it (if specified).
export function setupTestDatabase(conn, dbName, optPrimaryShard = null) {
    const newDb = conn.getSiblingDB(dbName);
    assert.commandWorked(newDb.dropDatabase());
    const createCmd = optPrimaryShard !== null
        ? {enablesharding: dbName, primaryShard: optPrimaryShard}
        : {enablesharding: dbName};

    assert.commandWorked(conn.adminCommand(createCmd));
    return newDb;
}
