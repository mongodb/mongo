/**
 * Ensures that the dropDatabase op can only be run if it's the single entry in an applyOps context.
 * Exercises that the operation can run even if it has to await replication of the intermediate
 * collection drops.
 *
 * @tags: [
 *   requires_replication,
 *   # dropDatabase can work in conjunction with other operations in 5.0.
 *   requires_fcv_53
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const dbName = "dbDrop";
const collName = "coll";
const cmdNss = dbName + ".$cmd";
const primaryDB = rst.getPrimary().getDB(dbName);

primaryDB.createCollection(collName);

const dropDatabaseOp = {
    op: "c",
    ns: cmdNss,
    o: {dropDatabase: 1}
};

// Verify that dropDatabase is only supported if it's the only op entry.
assert.commandFailedWithCode(
    primaryDB.adminCommand(
        {applyOps: [{op: "c", ns: cmdNss, o: {create: "collection"}}, dropDatabaseOp]}),
    6275900);

assert.contains(dbName, rst.getPrimary().getDBNames());
assert.sameMembers(primaryDB.getCollectionNames(), [collName]);

// TODO(SERVER-94154): Remove version check here.
const fcvDoc = primaryDB.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
if (MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version, "8.1") >= 0) {
    // Verify that dropDatabase is not supported inside nested applyOps.
    const createApplyOpsOplogEntry = (ops) => ({op: "c", ns: "admin.$cmd", o: {applyOps: ops}});

    assert.commandFailedWithCode(
        primaryDB.adminCommand({applyOps: [createApplyOpsOplogEntry([dropDatabaseOp])]}), 9585500);

    assert.commandFailedWithCode(
        primaryDB.adminCommand(
            {applyOps: [createApplyOpsOplogEntry([createApplyOpsOplogEntry([dropDatabaseOp])])]}),
        9585500);

    assert.contains(dbName, rst.getPrimary().getDBNames());
    assert.sameMembers(primaryDB.getCollectionNames(), [collName]);
}

// Run the dropDatabase op on a database with an existing collection so that we can exercise
// dropDatabase cleanly awaiting replication of the collection drop internally.
assert.commandWorked(primaryDB.adminCommand({applyOps: [dropDatabaseOp]}));

assert.eq(rst.getPrimary().getDBNames().indexOf(dbName), -1);

rst.stopSet();