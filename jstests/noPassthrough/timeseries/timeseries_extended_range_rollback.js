/**
 * Tests that time-series collection that require extended range support are properly recognized
 * after rollback.
 * @tags: [
 *   requires_replication,
 *   requires_mongobridge,
 * ]
 */
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const getExtendedRangeCount = (db) => {
    return assert.commandWorked(db.adminCommand({serverStatus: 1}))
        .catalogStats.timeseriesExtendedRange;
};

const collName = "test.standard";

// Operations that will be present on both nodes, before the common point.
let CommonOps = (node) => {
    const coll = node.getCollection(collName);
    const db = coll.getDB("test");

    assert.commandWorked(db.createCollection("standard", {timeseries: {timeField: "time"}}));
    assert.commandWorked(db.createCollection("extended", {timeseries: {timeField: "time"}}));
    assert.commandWorked(db.standard.insert({time: ISODate("1980-01-01T00:00:00.000Z")}, {w: 2}));
    assert.commandWorked(db.extended.insert({time: ISODate("2040-01-01T00:00:00.000Z")}, {w: 2}));
};

// Operations that will be performed on the rollback node past the common point.
let RollbackOps = (node) => {
    const coll = node.getCollection(collName);
    const db = coll.getDB("test");

    assert.commandWorked(db.createCollection("extra", {timeseries: {timeField: "time"}}));
};

// Set up Rollback Test.
const rollbackTest = new RollbackTest();
const primary = rollbackTest.getPrimary();
const secondary = rollbackTest.getSecondary();
assert.eq(undefined, getExtendedRangeCount(primary));
assert.eq(undefined, getExtendedRangeCount(secondary));
CommonOps(primary);

// Make sure the collections got flagged properly during the initial write.
assert(checkLog.checkContainsWithCountJson(
    primary, 6679402, {"nss": "test.standard", "timeField": "time"}, 0));
assert(checkLog.checkContainsWithCountJson(
    secondary, 6679402, {"nss": "test.standard", "timeField": "time"}, 0));
assert(checkLog.checkContainsWithCountJson(
    primary, 6679402, {"nss": "test.extended", "timeField": "time"}, 1));
assert(checkLog.checkContainsWithCountJson(
    secondary, 6679402, {"nss": "test.extended", "timeField": "time"}, 1));

assert.eq(1, getExtendedRangeCount(primary));
assert.eq(1, getExtendedRangeCount(secondary));

const rollbackNode = rollbackTest.transitionToRollbackOperations();
RollbackOps(rollbackNode);

rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations({skipDataConsistencyChecks: true});

// Make sure the collections get flagged properly again during rollback.
assert.eq(1, getExtendedRangeCount(rollbackNode));

// As of SERVER-86451, time-series inconsistencies detected during validation
// will error in testing, instead of being warnings. In this case,
// validation on shutdown would fail, where before only a warning would be thrown.
// TODO SERVER-87065: Look into re-enabling validation on shutdown.
rollbackTest.stop(null, true);
