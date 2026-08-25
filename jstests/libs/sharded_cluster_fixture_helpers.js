/**
 * High-level helper functions to support the interaction with the shards and routers of
 * core_sharding tests.
 */

function createCollectionAndInsertDocuments(testDb, collName, documents) {
    assert.commandWorked(testDb[collName].insert(documents));
}

// Drops and recreates dbName and assigns optPrimaryShard to it (if specified).
function setupTestDatabase(conn, dbName, optPrimaryShard = null) {
    const newDb = conn.getSiblingDB(dbName);
    assert.commandWorked(newDb.dropDatabase());
    const createCmd = optPrimaryShard !== null
        ? {enablesharding: dbName, primaryShard: optPrimaryShard}
        : {enablesharding: dbName};

    assert.commandWorked(conn.adminCommand(createCmd));
    return newDb;
}
