// Contains helper for checking format version of unique indexes.

/**
 * Verifies that all unique indexes belonging to all collections on all databases on the server
 * are in correct data format version.
 */
function checkUniqueIndexFormatVersion(adminDB) {
    // Data format version is WiredTiger specific and not required to be tested for other
    // storage engines.
    const isWiredTiger =
        assert.commandWorked(adminDB.serverStatus()).storageEngine.name === "wiredTiger";
    if (!isWiredTiger)
        return;

    res = assert.commandWorked(adminDB.runCommand({"listDatabases": 1}));
    let databaseList = res.databases;

    databaseList.forEach(function(database) {
        let currentDatabase = adminDB.getSiblingDB(database.name);
        // Get the list of collections including collections that are pending drop. This is to
        // ensure that every unique index has the correct format version.
        let collectionsWithPending =
            currentDatabase.runCommand("listCollections", {includePendingDrops: true})
                .cursor.firstBatch;
        collectionsWithPending.forEach(function(c) {
            let currentCollection = currentDatabase.getCollection(c.name);
            currentCollection.getIndexes().forEach(function(index) {
                if (index.unique) {
                    let ifv = currentCollection.aggregate({$collStats: {storageStats: {}}})
                                  .next()
                                  .storageStats.indexDetails[index.name]
                                  .metadata.formatVersion;
                    if (index.v === 2) {
                        assert.eq(
                            ifv,
                            12,
                            "Expected index format version 12 for unique index: " + tojson(index));
                    } else {
                        assert.eq(
                            ifv,
                            11,
                            "Expected index format version 11 for unique index: " + tojson(index));
                    }
                }
            });
        });
    });
}
