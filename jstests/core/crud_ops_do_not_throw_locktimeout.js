/**
 * Tests that CRUD operations do not throw lock timeouts outside of transactions.
 *
 * @tags: [assumes_against_mongod_not_mongos,
 *         assumes_read_concern_unchanged,
 *         assumes_write_concern_unchanged]
 */
(function() {
"use strict";

load("jstests/libs/curop_helpers.js");
load('jstests/libs/parallel_shell_helpers.js');

const coll = db[jsTestName()];
coll.drop();

const doc = {
    _id: 1
};
assert.commandWorked(coll.insert(doc));

const failpoint = 'hangAfterDatabaseLock';
assert.commandWorked(db.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn"}));

jsTestLog("Starting collMod that will block");

const awaitBlockingDDL =
    startParallelShell(funWithArgs(function(collName) {
                           assert.commandWorked(db.runCommand({collMod: collName}));
                       }, coll.getName()), db.getMongo().port);

jsTestLog("Waiting for collMod to acquire a database lock");
waitForCurOpByFailPointNoNS(db, failpoint);

// Each of the following operations should time out trying to acquire the collection lock, which the
// collMod is holding in mode X.
jsTestLog("Testing CRUD op timeouts");

const failureTimeoutMS = 1 * 1000;

assert.commandFailedWithCode(
    db.runCommand({insert: coll.getName(), documents: [{_id: 2}], maxTimeMS: failureTimeoutMS}),
    ErrorCodes.MaxTimeMSExpired);

assert.commandFailedWithCode(db.runCommand({find: coll.getName(), maxTimeMS: failureTimeoutMS}),
                             ErrorCodes.MaxTimeMSExpired);

assert.commandFailedWithCode(db.runCommand({
    update: coll.getName(),
    updates: [{q: doc, u: {$set: {b: 1}}}],
    maxTimeMS: failureTimeoutMS
}),
                             ErrorCodes.MaxTimeMSExpired);

assert.commandFailedWithCode(
    db.runCommand(
        {delete: coll.getName(), deletes: [{q: doc, limit: 1}], maxTimeMS: failureTimeoutMS}),
    ErrorCodes.MaxTimeMSExpired);

assert.commandFailedWithCode(db.runCommand({
    findAndModify: coll.getName(),
    query: {q: doc},
    update: {$set: {b: 2}},
    maxTimeMS: failureTimeoutMS
}),
                             ErrorCodes.MaxTimeMSExpired);

assert.commandFailedWithCode(db.runCommand({
    findAndModify: coll.getName(),
    query: {q: doc},
    remove: true,
    maxTimeMS: failureTimeoutMS
}),
                             ErrorCodes.MaxTimeMSExpired);

jsTestLog("Waiting for threads to join");
assert.commandWorked(db.adminCommand({configureFailPoint: failpoint, mode: "off"}));
awaitBlockingDDL();

assert.sameMembers(coll.find().toArray(), [doc]);
})();
