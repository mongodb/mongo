/**
 * Tests that an unprepared transaction can be rolled back.
 * @tags: [
 *   requires_replication,
 * ]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {checkRollbackFiles} from "jstests/replsets/libs/rollback_files.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

// Operations that will be present on both nodes, before the common point.
const dbName = "test";
const collName = "test.t";
const collNameShort = "t";
let CommonOps = (node) => {
    const coll = node.getCollection(collName);
    assert.commandWorked(coll.insert({_id: 0}));
};

// Operations that will be performed on the rollback node past the common point.
let RollbackOps = (node) => {
    const session = node.startSession();
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(collNameShort);
    session.startTransaction();
    assert.commandWorked(sessionColl.insert({_id: "a"}));
    assert.commandWorked(sessionColl.insert({_id: "b"}));
    assert.commandWorked(sessionColl.insert({_id: "c"}));
    assert.commandWorked(session.commitTransaction_forTesting());
    session.endSession();
};

// Set up Rollback Test.
const rollbackTest = new RollbackTest();

CommonOps(rollbackTest.getPrimary());

const rollbackNode = rollbackTest.transitionToRollbackOperations();
RollbackOps(rollbackNode);

// Wait for rollback to finish.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

// Check collection count.
const primary = rollbackTest.getPrimary();
const coll = primary.getCollection(collName);
assert.eq(1, coll.find().itcount());
assert.eq(1, coll.count());

// Confirm that the rollback wrote deleted documents to a file.
const replTest = rollbackTest.getTestFixture();
const expectedDocs = [{_id: "a"}, {_id: "b"}, {_id: "c"}];

const uuid = getUUIDFromListCollections(rollbackTest.getPrimary().getDB(dbName), collNameShort);
checkRollbackFiles(replTest.getDbPath(rollbackNode), collName, uuid, expectedDocs);

rollbackTest.stop();
