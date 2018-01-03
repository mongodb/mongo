// Contains helpers for checking UUIDs on collections.

/**
 * Verifies that all collections on all databases on the server with admin database adminDB have
 * UUIDs if isDowngrade is false and don't have UUIDs if isDowngrade is true.
 */
function checkCollectionUUIDs(adminDB, isDowngrade) {
    let databaseList = adminDB.runCommand({"listDatabases": 1}).databases;

    databaseList.forEach(function(database) {
        let currentDatabase = adminDB.getSiblingDB(database.name);
        let collectionInfos = currentDatabase.getCollectionInfos();
        for (let i = 0; i < collectionInfos.length; i++) {
            // Always skip system.indexes due to SERVER-30500.
            if (collectionInfos[i].name == "system.indexes") {
                continue;
            }
            if (isDowngrade) {
                assert(!collectionInfos[i].info.uuid,
                       "Unexpected uuid for collection: " + tojson(collectionInfos[i]));
            } else {
                assert(collectionInfos[i].info.uuid,
                       "Expect uuid for collection: " + tojson(collectionInfos[i]));
            }
        }
    });
}
