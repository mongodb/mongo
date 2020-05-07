/**
 * This test ensures that indexes created by running applyOps are both successful and replicated
 * correctly (see SERVER-31435).
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

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
           "did not find the index '" + indexName +
               "' amongst the collection indexes: " + tojson(indexes));
};

let ensureOplogEntryExists = function(localDB, indexName) {
    // Make sure the oplog entry for index creation exists in the oplog.
    let cmd = {find: "oplog.rs"};
    let res = localDB.runCommand(cmd);
    assert.commandWorked(res, "could not run " + tojson(cmd));
    let cursor = new DBCommandCursor(localDB, res);
    let errMsg = "expected more data from command " + tojson(cmd) + ", with result " + tojson(res);
    assert(cursor.hasNext(), errMsg);
    let oplog = localDB.getCollection("oplog.rs");

    // If two phase index builds are enabled, index creation will show up in the oplog as a pair of
    // startIndexBuild and commitIndexBuild oplog entries rather than a single createIndexes entry.
    let query = {
        $and: [
            {"o.startIndexBuild": {$exists: true}},
            {"o.indexes.0.name": indexName},
        ],
    };
    let resCursor = oplog.find(query);
    assert.eq(resCursor.count(),
              1,
              "Expected the query " + tojson(query) + " to return exactly 1 document");
    query = {$and: [{"o.commitIndexBuild": {$exists: true}}, {"o.indexes.0.name": indexName}]};
    resCursor = oplog.find(query);
    assert.eq(resCursor.count(),
              1,
              "Expected the query " + tojson(query) + " to return exactly 1 document");
};

let rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

let collName = "create_indexes_col";
let dbName = "create_indexes_db";

let primary = rst.getPrimary();
let primaryTestDB = primary.getDB(dbName);
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

// It is not possible to test createIndexes in applyOps with two-phase-index-builds support because
// that command is not accepted by applyOps in that mode.
assert.commandFailedWithCode(res, ErrorCodes.CommandNotSupported);

rst.stopSet();
}());
