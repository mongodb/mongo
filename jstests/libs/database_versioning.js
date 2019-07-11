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
        // Only compare the _id and version fields.

        assert.neq(null, authoritativeEntry._id);
        assert.neq(null, cacheEntry._id);
        assert.eq(authoritativeEntry._id, cacheEntry._id);

        assert.neq(null, authoritativeEntry.version);
        assert.neq(null, cacheEntry.version);
        assert.docEq(authoritativeEntry.version, cacheEntry.version);
    }
}
