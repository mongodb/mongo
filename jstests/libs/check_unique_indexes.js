// Contains helper for checking format version of unique indexes.

/**
 * Verifies that all unique indexes belonging to all collections on all databases on the server
 * are in correct data format version.
 */
function checkUniqueIndexFormatVersion(adminDB, currentFCV) {
    let getParameterResult =
        adminDB.runCommand({getParameter: 1, createTimestampSafeUniqueIndex: 1});
    assert.commandWorked(getParameterResult);
    // TODO(SERVER-34489) Remove this check when upgrade/downgrade is ready.
    if (!getParameterResult.createTimestampSafeUniqueIndex)
        return;

    // Data format version is WiredTiger specific and not required to be tested for other storage
    // engines.
    const isWiredTiger =
        assert.commandWorked(adminDB.serverStatus()).storageEngine.name === "wiredTiger";
    if (!isWiredTiger)
        return;

    let res = assert.commandWorked(adminDB.runCommand({"listDatabases": 1}));
    let databaseList = res.databases;

    databaseList.forEach(function(database) {
        let currentDatabase = adminDB.getSiblingDB(database.name);
        currentDatabase.getCollectionInfos().forEach(function(c) {
            let currentCollection = currentDatabase.getCollection(c.name);
            currentCollection.getIndexes().forEach(function(index) {
                if (index.unique) {
                    let ifv = currentCollection.aggregate({$collStats: {storageStats: {}}})
                                  .next()
                                  .storageStats.indexDetails[index.name]
                                  .metadata.formatVersion;
                    // Unique indexes are expected to have new format version only with latest FCV.
                    if (index.v === 2) {
                        if (currentFCV === latestFCV)
                            assert.eq(ifv,
                                      10,
                                      "Expected index format version 10 for unique index: " +
                                          tojson(index));
                        else
                            assert.eq(ifv,
                                      8,
                                      "Expected index format version 8 for unique index: " +
                                          tojson(index));
                    } else {
                        if (currentFCV === latestFCV)
                            assert.eq(ifv,
                                      9,
                                      "Expected index format version 9 for unique index: " +
                                          tojson(index));
                        else
                            assert.eq(ifv,
                                      6,
                                      "Expected index format version 6 for unique index: " +
                                          tojson(index));
                    }
                }
            });
        });
    });
}
