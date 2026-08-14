/**
 * High-level helper functions to support the interaction with the shards and routers of
 * core_sharding tests.
 */

export function createCollectionAndInsertDocuments(collectionType, testDb, collName, documents, dataShard) {
    if (collectionType === "unsplittable") {
        assert.commandWorked(
            testDb.runCommand({
                createUnsplittableCollection: collName,
                dataShard: dataShard.shardName,
            }),
        );
    }
    assert.commandWorked(testDb[collName].insert(documents));
}

// Drops and recreates dbName and assigns optPrimaryShard to it (if specified).
export function setupTestDatabase(conn, dbName, optPrimaryShard = null) {
    const newDb = conn.getSiblingDB(dbName);
    assert.commandWorked(newDb.dropDatabase());
    const createCmd =
        optPrimaryShard !== null ? {enablesharding: dbName, primaryShard: optPrimaryShard} : {enablesharding: dbName};

    assert.commandWorked(conn.adminCommand(createCmd));
    return newDb;
}

// Basic check of the tracking state for a namespace on the sharding catalog.
export function verifyCollectionTrackingState(conn, nss, expectedToBeTracked, expectedToBeUnsplittable = false) {
    const configDB = conn.getSiblingDB("config");
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
