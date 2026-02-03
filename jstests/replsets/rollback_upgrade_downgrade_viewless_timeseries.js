/**
 * Tests rollback of the upgradeDowngradeViewlessTimeseries oplog entry type.
 * TODO(SERVER-114573): Remove this test once 9.0 becomes lastLTS.
 *
 * @tags: [
 *   requires_mongobridge,
 *   requires_timeseries,
 *   featureFlagCreateViewlessTimeseriesCollections,
 *   multiversion_incompatible,
 * ]
 */

import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {getTimeseriesBucketsColl} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const testName = jsTestName();
const dbName = testName;
const collName = "tscoll";

// Operations that will be present on both nodes, before the common point.
const CommonOps = (node) => {
    const testDb = node.getDB(dbName);

    // Create a viewless timeseries collection.
    assert.commandWorked(testDb.createCollection(collName, {timeseries: {timeField: "t"}}));
    testDb[collName].insertOne({t: ISODate()});

    // To downgrade it to viewful format, the viewless timeseries feature flag must be disabled.
    // Start a FCV downgrade to disable it, but fail before it is actually downgraded by setFCV.
    const fp = configureFailPoint(node, "failDowngrading");
    assert.commandFailedWithCode(
        node.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        549181,
    );
    fp.off();
};

// Operations that will be performed on the rollback node past the common point.
const RollbackOps = (node) => {
    const testDb = node.getDB(dbName);

    // Downgrade the collection from viewless to viewful format
    // (via the applyOps command, since otherwise only setFCV can do the downgrade).
    assert.commandWorked(
        testDb.adminCommand({
            applyOps: [
                {
                    op: "c",
                    ns: dbName + ".$cmd",
                    o: {upgradeDowngradeViewlessTimeseries: collName, isUpgrade: false},
                    ui: node.getDB(dbName)[collName].getUUID(),
                },
            ],
        }),
    );

    // The rollback node should have a viewful timeseries collection.
    const coll = testDb[collName];
    assert(coll.exists());
    assert(getTimeseriesBucketsColl(coll).exists());
    assert.eq(1, coll.countDocuments({}));
};

// Set up Rollback Test.
const rollbackTest = new RollbackTest(testName);
CommonOps(rollbackTest.getPrimary());

// Perform the operations that will be rolled back.
const rollbackNode = rollbackTest.transitionToRollbackOperations();
RollbackOps(rollbackNode);

// Wait for rollback to finish.
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

// All nodes should have a viewless timeseries collection.
for (const node of rollbackTest.getTestFixture().nodes) {
    const coll = node.getDB(dbName)[collName];
    assert(coll.exists());
    assert(!getTimeseriesBucketsColl(coll).exists());
    assert.eq(1, coll.countDocuments({}));
}

rollbackTest.stop();
