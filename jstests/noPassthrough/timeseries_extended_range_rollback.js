/**
 * Tests that time-series collection that require extended range support are properly recognized
 * after rollback.
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/replsets/libs/rollback_test.js');

const collName = "test.standard";

// Operations that will be present on both nodes, before the common point.
let CommonOps = (node) => {
    const coll = node.getCollection(collName);
    const db = coll.getDB();

    assert.commandWorked(db.createCollection("standard", {timeseries: {timeField: "time"}}));
    assert.commandWorked(db.createCollection("extended", {timeseries: {timeField: "time"}}));
    assert.commandWorked(db.standard.insert({time: ISODate("1980-01-01T00:00:00.000Z")}));
    assert.commandWorked(db.extended.insert({time: ISODate("2040-01-01T00:00:00.000Z")}));
};

// Operations that will be performed on the rollback node past the common point.
let RollbackOps = (node) => {
    const coll = node.getCollection(collName);
    const db = coll.getDB();

    assert.commandWorked(db.createCollection("extra", {timeseries: {timeField: "time"}}));
};

// Set up Rollback Test.
const rollbackTest = new RollbackTest();
CommonOps(rollbackTest.getPrimary());

const rollbackNode = rollbackTest.transitionToRollbackOperations();
// Make sure the collections got flagged properly during the initial write.
checkLog.checkContainsWithCountJson(
    rollbackNode, 6679401, {"ns": "test.standard"}, 0, "WARNING", true);
checkLog.checkContainsWithCountJson(
    rollbackNode, 6679401, {"ns": "test.extended"}, 1, "WARNING", true);

RollbackOps(rollbackNode);

rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

// Make sure the collections get flagged properly again during rollback.
checkLog.checkContainsWithCountJson(
    rollbackNode, 6679401, {"ns": "test.standard"}, 0, "WARNING", true);
checkLog.checkContainsWithCountJson(
    rollbackNode, 6679401, {"ns": "test.extended"}, 2, "WARNING", true);

rollbackTest.stop();
})();
