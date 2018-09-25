/**
 * This test ensures that indexes created by running applyOps are both successful and replicated
 * correctly (see SERVER-31435).
 */
(function() {
    "use strict";
    let ensureIndexExists = function(testDB, collName, indexName, expectedNumIndexes) {
        let cmd = {listIndexes: collName};
        let res = testDB.runCommand(cmd);
        assert.commandWorked(res, "could not run " + tojson(cmd));
        let indexes = new DBCommandCursor(testDB, res).toArray();

        assert.eq(indexes.length, expectedNumIndexes);

        let foundIndex = false;
        for (let i = 0; i < indexes.length; ++i) {
            if (indexes[i].name == indexName) {
                foundIndex = true;
            }
        }
        assert(foundIndex,
               "did not find the index '" + indexName + "' amongst the collection indexes: " +
                   tojson(indexes));
    };

    let ensureOplogEntryExists = function(localDB, indexName) {
        // Make sure the oplog entry for index creation exists in the oplog.
        let cmd = {find: "oplog.rs"};
        let res = localDB.runCommand(cmd);
        assert.commandWorked(res, "could not run " + tojson(cmd));
        let cursor = new DBCommandCursor(localDB, res);
        let errMsg =
            "expected more data from command " + tojson(cmd) + ", with result " + tojson(res);
        assert(cursor.hasNext(), errMsg);
        let oplog = localDB.getCollection("oplog.rs");
        let query = {$and: [{"o.createIndexes": {$exists: true}}, {"o.name": indexName}]};
        let resCursor = oplog.find(query);
        assert.eq(resCursor.count(),
                  1,
                  "Expected the query " + tojson(query) + " to return exactly 1 document");
    };

    let rst = new ReplSetTest({nodes: 3});
    rst.startSet();
    rst.initiate();

    let collName = "create_indexes_col";
    let dbName = "create_indexes_db";

    let primaryTestDB = rst.getPrimary().getDB(dbName);
    let cmd = {"create": collName};
    let res = primaryTestDB.runCommand(cmd);
    assert.commandWorked(res, "could not run " + tojson(cmd));
    rst.awaitReplication();

    // Create an index via the applyOps command with the createIndexes command format and make sure
    // it exists.
    let uuid = primaryTestDB.getCollectionInfos()[0].info.uuid;
    let cmdFormatIndexNameA = "a_1";
    cmd = {
        applyOps: [{
            op: "c",
            ns: dbName + "." + collName,
            ui: uuid,
            o: {createIndexes: collName, v: 2, key: {a: 1}, name: cmdFormatIndexNameA}
        }]
    };
    res = primaryTestDB.runCommand(cmd);
    assert.commandWorked(res, "could not run " + tojson(cmd));
    rst.awaitReplication();
    ensureIndexExists(primaryTestDB, collName, cmdFormatIndexNameA, 2);

    // Same as directly above, but ensure that applyOps createIndexes can work without a uuid.
    let cmdFormatIndexNameB = "b_1";
    cmd = {
        applyOps: [{
            op: "c",
            ns: dbName + "." + collName,
            o: {createIndexes: collName, v: 2, key: {b: 1}, name: cmdFormatIndexNameB}
        }]
    };
    res = primaryTestDB.runCommand(cmd);
    assert.commandWorked(res, "could not run " + tojson(cmd));
    rst.awaitReplication();
    ensureIndexExists(primaryTestDB, collName, cmdFormatIndexNameB, 3);

    let localDB = rst.getPrimary().getDB("local");
    ensureOplogEntryExists(localDB, cmdFormatIndexNameA);
    ensureOplogEntryExists(localDB, cmdFormatIndexNameB);

    // Make sure the indexes were replicated to the secondaries.
    let secondaries = rst.getSecondaries();
    for (let j = 0; j < secondaries.length; j++) {
        let secondaryTestDB = secondaries[j].getDB(dbName);
        ensureIndexExists(secondaryTestDB, collName, cmdFormatIndexNameA, 3);
        ensureIndexExists(secondaryTestDB, collName, cmdFormatIndexNameB, 3);
    }

    // Create an index by inserting into system.indexes in applyOps.
    let insertFormatIndexName = "c_1";
    cmd = {
        applyOps: [{
            "op": "i",
            "ns": dbName + ".system.indexes",
            "o": {
                ns: dbName + "." + collName,
                key: {c: 1},
                name: insertFormatIndexName,
            }
        }]
    };
    res = primaryTestDB.adminCommand(cmd);
    assert.commandWorked(res, "could not run " + tojson(cmd));
    rst.awaitReplication();
    ensureIndexExists(primaryTestDB, collName, insertFormatIndexName, 4);

    // Make sure the index was replicated to the secondaries.
    secondaries = rst.getSecondaries();
    for (let j = 0; j < secondaries.length; j++) {
        let secondaryTestDB = secondaries[j].getDB(dbName);
        ensureIndexExists(secondaryTestDB, collName, insertFormatIndexName, 4);
    }

    localDB = rst.getPrimary().getDB("local");
    ensureOplogEntryExists(localDB, insertFormatIndexName);

    rst.stopSet();
}());
