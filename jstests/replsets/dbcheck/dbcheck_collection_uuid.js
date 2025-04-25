/**
 * Tests the collectionUUID field in dbCheck health log entries.
 *
 * @tags: [
 *   requires_fcv_71
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {checkHealthLog, resetAndInsert, runDbCheck} from "jstests/replsets/libs/dbcheck_utils.js";

const replSet = new ReplSetTest({
    name: jsTestName(),
    nodes: 2,
    nodeOptions: {setParameter: {dbCheckHealthLogEveryNBatches: 1}},
});
replSet.startSet();
replSet.initiate();

const dbName = "dbCheckCollectionUUID";
const collName = "dbCheckCollectionUUID-collection";
const primary = replSet.getPrimary();
const secondary = replSet.getSecondary();
const primaryHealthlog = primary.getDB("local").system.healthlog;
const secondaryHealthlog = secondary.getDB("local").system.healthlog;
const db = primary.getDB(dbName);

function healthLogCollectionUUID() {
    jsTestLog("Testing collectionUUID field in health log");

    const nDocs = 1000;
    const maxDocsPerBatch = 100;
    resetAndInsert(replSet, db, collName, nDocs);
    runDbCheck(replSet, db, collName, {maxDocsPerBatch: maxDocsPerBatch});

    // All entries in primay and secondary health log should include the correct collectionUUID.
    const collUUID = db.getCollectionInfos({name: collName})[0].info.uuid;
    const numExpected = nDocs / maxDocsPerBatch;
    let query = {operation: "dbCheckBatch", collectionUUID: collUUID};
    checkHealthLog(primaryHealthlog, query, numExpected);
    checkHealthLog(secondaryHealthlog, query, numExpected);

    // There are no dbCheckBatch health log entries without a collectionUUID.
    query = {operation: "dbCheckBatch", collectionUUID: {$exists: false}};
    checkHealthLog(primaryHealthlog, query, 0);
    checkHealthLog(secondaryHealthlog, query, 0);
}
healthLogCollectionUUID();

replSet.stopSet();
