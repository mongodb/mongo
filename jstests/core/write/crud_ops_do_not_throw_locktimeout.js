/**
 * Tests that CRUD operations do not throw lock timeouts outside of transactions.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_read_concern_unchanged,
 *   assumes_write_concern_unchanged,
 *   uses_parallel_shell,
 *   no_selinux,
 *   # Multi clients cannot share global fail points. When one client turns off a fail point, other
 *   # clients waiting on the fail point will get failed.
 *   multi_clients_incompatible,
 * ]
 */
import {waitForCurOpByFailPointNoNS} from "jstests/libs/curop_helpers.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

const coll = db[jsTestName()];
coll.drop();

const doc = {
    _id: 1
};
assert.commandWorked(coll.insert(doc));

// Figure out if lock-free reads is supported so we know the expected behavior later.
// Lock-free reads are only supported in server versions 4.9+
const maxWireVersion = assert.commandWorked(db.runCommand({isMaster: 1})).maxWireVersion;
const isLockFreeReadsEnabled = maxWireVersion >= 12 /* WIRE_VERSION_49 */;

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
const comment = extractUUIDFromObject(UUID());

assert.commandFailedWithCode(
    db.runCommand(
        {insert: coll.getName(), documents: [{_id: 2}], maxTimeMS: failureTimeoutMS, comment}),
    ErrorCodes.MaxTimeMSExpired);

// Reads are not blocked by MODE_X Collection locks with Lock Free Reads.
const findResult = db.runCommand({find: coll.getName(), maxTimeMS: failureTimeoutMS, comment});
if (isLockFreeReadsEnabled)
    assert.commandWorked(findResult);
else
    assert.commandFailedWithCode(findResult, ErrorCodes.MaxTimeMSExpired);

assert.commandFailedWithCode(db.runCommand({
    update: coll.getName(),
    updates: [{q: doc, u: {$set: {b: 1}}}],
    maxTimeMS: failureTimeoutMS,
    comment
}),
                             ErrorCodes.MaxTimeMSExpired);

assert.commandFailedWithCode(db.runCommand({
    delete: coll.getName(),
    deletes: [{q: doc, limit: 1}],
    maxTimeMS: failureTimeoutMS,
    comment
}),
                             ErrorCodes.MaxTimeMSExpired);

assert.commandFailedWithCode(db.runCommand({
    findAndModify: coll.getName(),
    query: doc,
    update: {$set: {b: 2}},
    maxTimeMS: failureTimeoutMS,
    comment
}),
                             ErrorCodes.MaxTimeMSExpired);

assert.commandFailedWithCode(db.runCommand({
    findAndModify: coll.getName(),
    query: doc,
    remove: true,
    maxTimeMS: failureTimeoutMS,
    comment
}),
                             ErrorCodes.MaxTimeMSExpired);

if (TestData.testingReplicaSetEndpoint) {
    // When using the replica set endpoint, each CRUD command above would fail MaxTimeMSExpired when
    // the router command has timed out, but it is possible that the shard command has not. If it
    // has not timed out, disabling the failpoint (below) would allow the command to execute and
    // cause the test to fail the assertion that none of the writes happened.
    assert.soon(() => {
        const docs = db.getSiblingDB('admin')
                         .aggregate([{$currentOp: {}}, {$match: {"command.comment": comment}}])
                         .toArray();
        return docs.length == 0;
    });
}

jsTestLog("Waiting for threads to join");
assert.commandWorked(db.adminCommand({configureFailPoint: failpoint, mode: "off"}));
awaitBlockingDDL();

assert.sameMembers(coll.find().toArray(), [doc]);
