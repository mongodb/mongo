/**
 * Test createIndexes while recursively locked in a nested applyOps.
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');

let rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

let collName = "col";
let dbName = "nested_apply_ops_create_indexes";

let primary = rst.getPrimary();
let primaryTestDB = primary.getDB(dbName);
let cmd = {"create": collName};
let res = primaryTestDB.runCommand(cmd);
assert.commandWorked(res, "could not run " + tojson(cmd));
rst.awaitReplication();

let uuid = primaryTestDB.getCollectionInfos()[0].info.uuid;
let cmdFormatIndexNameA = "a_1";
cmd = {
    applyOps: [{
        op: "c",
        ns: dbName + ".$cmd",
        ui: uuid,
        o: {
            applyOps: [{
                op: "c",
                ns: dbName + "." + collName,
                ui: uuid,
                o: {createIndexes: collName, v: 2, key: {a: 1}, name: cmdFormatIndexNameA}
            }]
        }
    }]
};
res = primaryTestDB.runCommand(cmd);

// It is not possible to test createIndexes in applyOps with two-phase-index-builds support because
// that command is not accepted by applyOps in that mode.
if (IndexBuildTest.supportsTwoPhaseIndexBuild(primary)) {
    assert.commandFailedWithCode(res, ErrorCodes.CommandNotSupported);
    rst.stopSet();
    return;
}

assert.commandWorked(res, "could not run " + tojson(cmd));
rst.awaitReplication();

const coll = primaryTestDB.getCollection(collName);
IndexBuildTest.assertIndexes(coll, 2, ['_id_', cmdFormatIndexNameA]);

rst.stopSet();
})();
