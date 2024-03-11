/**
 * Tests the behavior of dbCheck when operating on an older snapshot of the catalog. It simulates a
 * situation where a collection previously existed but was deleted and replaced by a view with the
 * same name. Additionally, the change was replicated before the dbCheck oplog operation.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    assertForDbCheckErrorsForAllNodes,
    awaitDbCheckCompletion,
    checkHealthLog,
    clearHealthLog,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

const replSet = new ReplSetTest({
    nodes: 3,
    nodeOptions: {
        setParameter: {dbCheckHealthLogEveryNBatches: 1},
    }
});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const secondary = replSet.getSecondary();
const primaryHealthlog = primary.getDB("local").system.healthlog;
const secondaryHealthlog = secondary.getDB("local").system.healthlog;
const dbName = 'test';
const db = primary.getDB(dbName);
const collName = 'coll';
const coll = db[collName];
const collName2 = 'coll2';
const coll2 = db[collName2];

// Turn off timestamp reaping.
assert.commandWorked(db.adminCommand({
    configureFailPoint: "WTPreserveSnapshotHistoryIndefinitely",
    mode: "alwaysOn",
}));

// Create the collection and insert one document.
assert.commandWorked(coll.insert({_id: 0}));
const collUUID = db.getCollectionInfos({name: collName})[0].info.uuid;
replSet.awaitReplication();
const fp = configureFailPoint(primary, "hangBeforeAddingDBCheckBatchToOplog");
clearHealthLog(replSet);
// Start dbCheck.
runDbCheck(replSet, db, collName, {});
// Wait until the dbCheck's finish the batch and blocked before adding the oplog.
fp.wait();

// Drop the collection and create a view with the same name and wait for it to be replicated to
// secondaries before the dbcheck oplog.
coll.drop();
assert.commandWorked(db.createView(collName, coll2.getName(), []));
replSet.awaitReplication();

// Let dbCheck continue.
fp.off();

// Ensure that dbCheck completes successfully on all nodes.
awaitDbCheckCompletion(replSet, db, true /*waitForHealthLogDbCheckStop*/)
assertForDbCheckErrorsForAllNodes(replSet, true /*assertForErrors*/, true /*assertForWarnings*/);

// Make sure the dbCheck has one batch with the one document we have inserted.
let query = {operation: "dbCheckBatch", collectionUUID: collUUID, "data.count": 1};
checkHealthLog(primaryHealthlog, query, 1);
checkHealthLog(secondaryHealthlog, query, 1);

replSet.stopSet();
