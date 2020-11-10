/**
 * This test ensures that indexes created by running applyOps are both successful and replicated
 * correctly (see SERVER-31435).
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

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
