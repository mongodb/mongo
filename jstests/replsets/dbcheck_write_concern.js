/**
 * Test the behavior of per-batch writeConcern in dbCheck.
 *
 * @tags: [
 *   # We need persistence as we temporarily restart nodes.
 *   requires_persistence,
 *   assumes_against_mongod_not_mongos,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {checkHealthLog, resetAndInsert, runDbCheck} from "jstests/replsets/libs/dbcheck_utils.js";

const replSet = new ReplSetTest({
    name: "dbCheckWriteConcern",
    nodes: 2,
    nodeOptions: {setParameter: {dbCheckHealthLogEveryNBatches: 1}},
    settings: {
        // Prevent the primary from stepping down when we temporarily shut down the secondary.
        electionTimeoutMillis: 120000
    }
});
replSet.startSet();
replSet.initiate();

const dbName = "dbCheck-writeConcern";
const collName = "test";
const primary = replSet.getPrimary();
const db = primary.getDB(dbName);
const coll = db[collName];
const healthlog = db.getSiblingDB('local').system.healthlog;

// Validate that w:majority behaves normally.
(function testWMajority() {
    // Insert 1000 docs and run a few small batches to ensure we wait for write concern between
    // each one.
    const nDocs = 1000;
    const maxDocsPerBatch = 100;
    resetAndInsert(replSet, db, collName, nDocs);

    const dbCheckParameters = {maxDocsPerBatch: maxDocsPerBatch};
    runDbCheck(replSet, db, collName, dbCheckParameters);

    // Confirm dbCheck logs the expected number of batches.
    checkHealthLog(
        healthlog, {operation: "dbCheckBatch", severity: "info"}, nDocs / maxDocsPerBatch);

    // Confirm there are no warnings or errors.
    checkHealthLog(healthlog, {operation: "dbCheckBatch", severity: "warning"}, 0);
    checkHealthLog(healthlog, {operation: "dbCheckBatch", severity: "error"}, 0);
})();

// Validate that w:2 behaves normally.
(function testW2() {
    // Insert 1000 docs and run a few small batches to ensure we wait for write concern between
    // each one.
    const nDocs = 1000;
    const maxDocsPerBatch = 100;
    const writeConcern = {w: 2};
    resetAndInsert(replSet, db, collName, nDocs);

    const dbCheckParameters = {maxDocsPerBatch: maxDocsPerBatch, batchWriteConcern: writeConcern};
    runDbCheck(replSet, db, collName, dbCheckParameters);

    // Confirm dbCheck logs the expected number of batches.
    checkHealthLog(
        healthlog, {operation: "dbCheckBatch", severity: "info"}, nDocs / maxDocsPerBatch);

    // Confirm there are no warnings or errors.
    checkHealthLog(healthlog, {operation: "dbCheckBatch", severity: "warning"}, 0);
    checkHealthLog(healthlog, {operation: "dbCheckBatch", severity: "error"}, 0);
})();

replSet.stopSet();
