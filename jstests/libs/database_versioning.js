// Provides methods for inspecting the in-memory and on-disk database caches on shard nodes.

function checkInMemoryDatabaseVersion(conn, dbName, expectedVersion) {
    const res = conn.adminCommand({getDatabaseVersion: dbName});
    assert.commandWorked(res);
    assert.docEq(res.dbVersion,
                 expectedVersion,
                 conn + " did not have expected in-memory database version for " + dbName);
}

function checkOnDiskDatabaseVersion(conn, dbName, authoritativeEntry) {
    const res = conn.getDB("config").runCommand({find: "cache.databases", filter: {_id: dbName}});
    assert.commandWorked(res);
    const cacheEntry = res.cursor.firstBatch[0];

    if (authoritativeEntry === undefined) {
        assert.eq(cacheEntry, undefined);
    } else {
        // Remove the 'enterCriticalSectionCounter' field, which is the only field the cache
        // entry should have but the authoritative entry does not.
        delete cacheEntry["enterCriticalSectionCounter"];
        assert.docEq(cacheEntry,
                     authoritativeEntry,
                     conn + " did not have expected on-disk database version for " + dbName);
    }
}
