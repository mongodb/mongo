
/**
 * High-level helper functions to support the interaction with the shards and routers of
 * core_sharding tests.
 */

function _getRandomElem(list) {
    return list[Math.floor(Math.random() * list.length)];
}

export function getShardDescriptors(conn) {
    return assert.commandWorked(conn.adminCommand({listShards: 1})).shards;
}

export function getShardNames(conn) {
    return getShardDescriptors(conn).map(shard => shard._id);
}

export function getNumShards(conn) {
    return getShardDescriptors(conn).length;
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

// Prepares a dbName for the correct execution of a test case.
export function setupDbName(conn, suffix) {
    const dbName = jsTestName() + '_' + suffix;
    conn.getSiblingDB(dbName).dropDatabase();
    return dbName;
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

// Basic check of the tracking state for a namespace on the sharding catalog.
export function verifyCollectionTrackingState(
    conn, nss, expectedToBeTracked, expectedToBeUnsplittable = false) {
    const configDB = conn.getSiblingDB('config');
    const matchingCollDocs = configDB.collections.find({_id: nss}).toArray();
    if (expectedToBeTracked) {
        assert.eq(1, matchingCollDocs.length);
        const unsplittable = matchingCollDocs[0].unsplittable ? true : false;
        assert.eq(expectedToBeUnsplittable, unsplittable);
        if (expectedToBeUnsplittable) {
            const numChunks = configDB.chunks.count({uuid: matchingCollDocs[0].uuid});
            assert.eq(1, numChunks);
        }
    } else {
        assert.eq(0, matchingCollDocs.length);
    }
}
