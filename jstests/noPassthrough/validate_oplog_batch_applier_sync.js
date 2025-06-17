/**
 * Tests that foreground validation on the secondary occurs in between batch boundaries of oplog
 * application and that trying to proceed with validation while we have yet to finish applying a
 * batch will result in validate being blocked due to waiting for a lock.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

const dbName = "test_validation";
const collName = "test_coll_validation";
const primaryDB = primary.getDB(dbName);
const secDB = secondary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

/*
 * Create some indexes and insert some data, so we can validate them more meaningfully.
 */
assert.commandWorked(primaryColl.createIndex({a: 1}));
assert.commandWorked(primaryColl.createIndex({b: 1}));
assert.commandWorked(primaryColl.createIndex({c: 1}));

const numDocs = 10;
for (let i = 0; i < numDocs; ++i) {
    assert.commandWorked(primaryColl.insert({a: i, b: i, c: i}));
}

rst.awaitReplication();
/* Enable the failpoint to stop oplog batch application before completion. */
let failPoint = "pauseBatchApplicationBeforeCompletion";
const fp = configureFailPoint(secDB, failPoint);

// Start oplog batch application.
const awaitOplog = startParallelShell(function() {
    const primaryDB = db.getSiblingDB('test_validation');
    const coll = primaryDB.getCollection('test_coll_validation');
    assert.commandWorked(coll.insert({a: 1, b: 1, c: 1}, {writeConcern: {w: 2}}));
}, primary.port);

fp.wait();

// Start validation on the secondary on a parallel shell.
const awaitValidate = startParallelShell(function() {
    const secDB = db.getSiblingDB('test_validation');
    const coll = secDB.getCollection('test_coll_validation');
    secDB.setSecondaryOk();
    const res = coll.validate();
    assert.commandWorked(res);
    assert(res.valid, "Validate cmd failed: " + tojson(res));
}, secondary.port);

// Ensure that the validation call is blocked on waiting for the lock.
assert.soon(() => {
    return secDB.currentOp().inprog.some(op => {
        return op.active && (op.command.validate === "test_coll_validation") && (op.waitingForLock);
    });
});

fp.off();
awaitOplog();
awaitValidate();

rst.stopSet();
