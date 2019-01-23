/**
 * For FCV 4.2, MongoDB uses a new internal format for unique indexes that is incompatible with 4.0.
 * The new format applies to both existing unique indexes as well as newly created/rebuilt unique
 * indexes. This helper function rebuilds all unique indexes on an instance after downgrading to FCV
 * 4.0, for backwards compatibility with binary 4.0. Because this is an internal change, the index
 * version is retained through rebuilding.
 */
function downgradeUniqueIndexes(db) {
    // Obtain a list of v:1 and v:2 unique indexes.
    const unique_idx_v1 = [];
    const unique_idx_v2 = [];
    db.adminCommand("listDatabases").databases.forEach(function(d) {
        let mdb = db.getSiblingDB(d.name);
        mdb.getCollectionInfos().forEach(function(c) {
            let currentCollection = mdb.getCollection(c.name);
            currentCollection.getIndexes().forEach(function(i) {
                if (i.unique) {
                    if (i.v === 1) {
                        unique_idx_v1.push(i);
                    } else {
                        unique_idx_v2.push(i);
                    }
                    return;
                }
            });
        });
    });
    // Drop and recreate all v:1 indexes
    for (let idx of unique_idx_v1) {
        let [dbName, collName] = idx.ns.split(".");
        let res = db.getSiblingDB(dbName).runCommand({dropIndexes: collName, index: idx.name});
        assert.commandWorked(res);
        res = db.getSiblingDB(dbName).runCommand({
            createIndexes: collName,
            indexes: [{"key": idx.key, "name": idx.name, "unique": true, "v": 1}]
        });
        assert.commandWorked(res);
    }

    // Drop and recreate all v:2 indexes
    for (let idx of unique_idx_v2) {
        let [dbName, collName] = idx.ns.split(".");
        let res = db.getSiblingDB(dbName).runCommand({dropIndexes: collName, index: idx.name});
        assert.commandWorked(res);
        res = db.getSiblingDB(dbName).runCommand({
            createIndexes: collName,
            indexes: [{"key": idx.key, "name": idx.name, "unique": true, "v": 2}]
        });
        assert.commandWorked(res);
    }
}
