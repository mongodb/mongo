/**
 * Tests the collectionUUID field in dbCheck health log entries.
 *
 * @tags: [
 *   requires_fcv_71
 * ]
 */

import {resetAndInsert, runDbCheck} from "jstests/replsets/libs/dbcheck_utils.js";

(function() {
"use strict";

const replSet = new ReplSetTest({
    name: jsTestName(),
    nodes: 2,
    nodeOptions: {setParameter: {dbCheckHealthLogEveryNBatches: 1}},
});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const dbName = "dbCheckCollectionUUID";
const collName = "dbCheckCollectionUUID-collection";
const primary = replSet.getPrimary();
const db = primary.getDB(dbName);
const healthlog = db.getSiblingDB('local').system.healthlog;

function healthLogCollectionUUID() {
    jsTestLog("Testing collectionUUID field in health log");

    const nDocs = 1000;
    const maxDocsPerBatch = 100;
    resetAndInsert(replSet, db, collName, nDocs);
    runDbCheck(replSet, db, collName, maxDocsPerBatch);

    // All entries in health log should include the correct collectionUUID.
    const collUUID = db.getCollectionInfos({name: collName})[0].info.uuid;
    assert.soon(function() {
        return healthlog.find({operation: "dbCheckBatch", collectionUUID: collUUID}).itcount() ==
            nDocs / maxDocsPerBatch;
    }, "dbCheck command didn't complete");

    // There are no dbCheckBatch health log entries without a collectionUUID.
    assert.eq(
        healthlog.find({operation: "dbCheckBatch", collectionUUID: {$exists: false}}).itcount(), 0);
}
healthLogCollectionUUID();

replSet.stopSet();
})();
