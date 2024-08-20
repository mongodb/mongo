/**
 * Tests that dbCheck ignores prepared updates on secondaries.
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {awaitDbCheckCompletion, checkHealthLog} from "jstests/replsets/libs/dbcheck_utils.js";

const replSet = new ReplSetTest({nodes: 2});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const secondary = replSet.getSecondary();

const dbName = 'test';
const db = primary.getDB(dbName);
const collName = 'coll';
const coll = db[collName];

assert.commandWorked(coll.insert({_id: 0, a: "first"}));

// Stop dbCheck after it scans but before it writes the oplog entry.
const failPoint = configureFailPoint(primary, "hangBeforeDbCheckLogOp");

const awaitDbCheck = startParallelShell(() => {
    assert.commandWorked(db.runCommand({dbCheck: 'coll'}));
}, primary.port);
failPoint.wait();

// Insert a document in a prepared state in the range that dbCheck has just validated.
const session = primary.startSession();
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB[collName];
session.startTransaction();
assert.commandWorked(sessionColl.insert({_id: 1}));

PrepareHelpers.prepareTransaction(session);

// Let dbCheck replicate to the secondary and ensure it does not hit a prepare conflict.
failPoint.off();
awaitDbCheck();

awaitDbCheckCompletion(replSet, db);

// We should not find inconsistencies on any node.
[primary, secondary].forEach((node) => {
    checkHealthLog(node.getDB('local').system.healthlog, {severity: "error"}, 0);
});

session.abortTransaction();

replSet.stopSet(undefined /* signal */, false /* forRestart */);
