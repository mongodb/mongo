/**
 * Tests that
 * - the recipient rejects all reads between when cloning is done and the rejectReadsBeforeTimestamp
 * - the recipient rejects only reads with atClusterTime <
 *   rejectReadsBeforeTimestamp after rejectReadsBeforeTimestamp is reached.
 * - if the migration aborts before the recipient sets a rejectReadsBeforeTimestamp, the recipient
 *   keeps rejecting all reads until the state doc is marked as garbage collectable.
 * - if the migration aborts after rejectReadsBeforeTimestamp is set, the recipient keeps rejecting
 *   reads with atClusterTime < rejectReadsBeforeTimestamp until the state doc is garbage collected.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/replsets/rslib.js");

const kCollName = "testColl";
const kTenantDefinedDbName = "0";

function runCommand(db, cmd, expectedError) {
    const res = db.runCommand(cmd);

    if (expectedError) {
        assert.commandFailedWithCode(res, expectedError, tojson(cmd));
        if (expectedError == ErrorCodes.SnapshotTooOld) {
            // Verify that SnapshotTooOld error is due to migration conflict not due to the read
            // timestamp being older than the oldest available timestamp.
            assert.eq(res.errmsg, "Tenant read is not allowed before migration completes");
        }
    } else {
        assert.commandWorked(res);
    }

    if (cmd.lsid) {
        const notRejectReadsFp = configureFailPoint(db, "tenantMigrationRecipientNotRejectReads");
        assert.commandWorked(db.runCommand({killSessions: [cmd.lsid]}));
        notRejectReadsFp.off();
    }
}

/**
 * Tests that the recipient starts rejecting all reads after cloning is done.
 */
function testRejectAllReadsAfterCloningDone({testCase, dbName, collName, tenantMigrationTest}) {
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    const donorRst = tenantMigrationTest.getDonorRst();
    const recipientRst = tenantMigrationTest.getRecipientRst();
    const recipientPrimary = recipientRst.getPrimary();

    let beforeFetchingTransactionsFp = configureFailPoint(
        recipientPrimary, "fpBeforeFetchingCommittedTransactions", {action: "hang"});

    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);
    const runMigrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
    runMigrationThread.start();
    beforeFetchingTransactionsFp.wait();

    // Wait for the write to mark cloning as done to be replicated to all nodes.
    recipientRst.awaitReplication();

    const nodes = testCase.isSupportedOnSecondaries ? recipientRst.nodes : [recipientPrimary];
    nodes.forEach(node => {
        const command = testCase.requiresReadTimestamp
            ? testCase.command(collName, getLastOpTime(node).ts)
            : testCase.command(collName);
        const db = node.getDB(dbName);
        runCommand(db, command, ErrorCodes.SnapshotTooOld);
    });

    beforeFetchingTransactionsFp.off();
    TenantMigrationTest.assertCommitted(runMigrationThread.returnData());
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
}

/**
 * Tests that after the recipient has reached the rejectReadsBeforeTimestamp and
 * after the migration commits, it only rejects reads with atClusterTime <
 * rejectReadsBeforeTimestamp.
 */
function testRejectOnlyReadsWithAtClusterTimeLessThanRejectReadsBeforeTimestamp(
    {testCase, dbName, collName, tenantMigrationTest}) {
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = donorRst.getPrimary();
    const recipientRst = tenantMigrationTest.getRecipientRst();
    const recipientPrimary = recipientRst.getPrimary();

    // Select a read timestamp < rejectReadsBeforeTimestamp.
    const preMigrationTimestamp = getLastOpTime(donorPrimary).ts;

    let waitForRejectReadsBeforeTsFp = configureFailPoint(
        recipientPrimary, "fpAfterWaitForRejectReadsBeforeTimestamp", {action: "hang"});

    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);
    const runMigrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
    runMigrationThread.start();
    waitForRejectReadsBeforeTsFp.wait();

    // Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
    // the oplog on all the secondaries. This is to ensure that snapshot reads on secondaries with
    // unspecified atClusterTime have read timestamp >= rejectReadsBeforeTimestamp.
    recipientRst.awaitLastOpCommitted();

    const recipientDoc =
        recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS).findOne({
            tenantId: tenantId
        });
    assert.lt(preMigrationTimestamp, recipientDoc.rejectReadsBeforeTimestamp);

    const nodes = testCase.isSupportedOnSecondaries ? recipientRst.nodes : [recipientPrimary];
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        if (testCase.requiresReadTimestamp) {
            runCommand(
                db, testCase.command(collName, preMigrationTimestamp), ErrorCodes.SnapshotTooOld);
            runCommand(
                db, testCase.command(collName, recipientDoc.rejectReadsBeforeTimestamp), null);
        } else {
            // Untimestamped reads are not rejected after the recipient has applied data past the
            // rejectReadsBeforeTimestamp. Snapshot reads with unspecified atClusterTime should have
            // read timestamp >= rejectReadsBeforeTimestamp so are also not rejected.
            runCommand(db, testCase.command(collName), null);
        }
    });

    waitForRejectReadsBeforeTsFp.off();
    TenantMigrationTest.assertCommitted(runMigrationThread.returnData());

    nodes.forEach(node => {
        const db = node.getDB(dbName);
        if (testCase.requiresReadTimestamp) {
            runCommand(
                db, testCase.command(collName, preMigrationTimestamp), ErrorCodes.SnapshotTooOld);
            runCommand(
                db, testCase.command(collName, recipientDoc.rejectReadsBeforeTimestamp), null);
        } else {
            // Untimestamped reads are not rejected after the recipient has committed. Snapshot
            // reads with unspecified atClusterTime should have read timestamp >=
            // rejectReadsBeforeTimestamp so are also not rejected.
            runCommand(db, testCase.command(collName), null);
        }
    });

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
}

/**
 * Tests that if the migration aborts before the recipient sets the rejectReadsBeforeTimestamp, the
 * recipient keeps rejecting all reads until the state doc is marked as garbage collectable.
 */
function testDoNotRejectReadsAfterMigrationAbortedBeforeReachingRejectReadsBeforeTimestamp(
    {testCase, dbName, collName, tenantMigrationTest}) {
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const recipientRst = tenantMigrationTest.getRecipientRst();
    const recipientPrimary = recipientRst.getPrimary();

    // Force the recipient to abort the migration right before it responds to the first
    // recipientSyncData (i.e. before it receives returnAfterReachingTimestamp in the second
    // recipientSyncData).
    let abortFp = configureFailPoint(recipientPrimary,
                                     "fpBeforeFulfillingDataConsistentPromise",
                                     {action: "stop", stopErrorCode: ErrorCodes.InternalError});
    TenantMigrationTest.assertAborted(
        tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: false}));
    abortFp.off();

    const nodes = testCase.isSupportedOnSecondaries ? recipientRst.nodes : [recipientPrimary];
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        if (testCase.requiresReadTimestamp) {
            runCommand(
                db, testCase.command(collName, getLastOpTime(node).ts), ErrorCodes.SnapshotTooOld);
        } else {
            runCommand(db, testCase.command(collName), ErrorCodes.SnapshotTooOld);
        }
    });

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    // Wait for the write to mark the state doc as garbage collectable to be replicated to all
    // nodes.
    recipientRst.awaitReplication();

    nodes.forEach(node => {
        const db = node.getDB(dbName);
        if (testCase.requiresReadTimestamp) {
            runCommand(db, testCase.command(collName, getLastOpTime(node).ts), null);
        } else {
            runCommand(db, testCase.command(collName), null);
        }
    });
}

/**
 * Tests if the migration aborts after rejectReadsBeforeTimestamp is set, the recipient keeps
 * rejecting reads with atClusterTime < rejectReadsBeforeTimestamp until the state doc is garbage
 * collected.
 */
function testDoNotRejectReadsAfterMigrationAbortedAfterReachingRejectReadsBeforeTimestamp(
    {testCase, dbName, collName, tenantMigrationTest}) {
    const tenantId = dbName.split('_')[0];
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId,
    };

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = donorRst.getPrimary();
    const recipientRst = tenantMigrationTest.getRecipientRst();
    const recipientPrimary = recipientRst.getPrimary();

    const setParametersCmd = {
        setParameter: 1,
        // Set the delay before a state doc is garbage collected to be short to speed up the test.
        tenantMigrationGarbageCollectionDelayMS: 3 * 1000,
        ttlMonitorSleepSecs: 1,
    };
    donorRst.nodes.forEach(node => {
        assert.commandWorked(node.adminCommand(setParametersCmd));
    });
    recipientRst.nodes.forEach(node => {
        assert.commandWorked(node.adminCommand(setParametersCmd));
    });

    // Select a read timestamp < rejectReadsBeforeTimestamp.
    const preMigrationTimestamp = getLastOpTime(donorPrimary).ts;

    // Force the donor to abort the migration right after the recipient responds to the second
    // recipientSyncData (i.e. after it has reached the returnAfterReachingTimestamp).
    let abortFp =
        configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");
    TenantMigrationTest.assertAborted(
        tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: false}));
    abortFp.off();

    // Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
    // the oplog on all the secondaries. This is to ensure that snapshot reads on secondaries with
    // unspecified atClusterTime have read timestamp >= rejectReadsBeforeTimestamp.
    recipientRst.awaitLastOpCommitted();

    const recipientDoc =
        recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS).findOne({
            tenantId: tenantId
        });

    const nodes = testCase.isSupportedOnSecondaries ? recipientRst.nodes : [recipientPrimary];
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        if (testCase.requiresReadTimestamp) {
            runCommand(
                db, testCase.command(collName, preMigrationTimestamp), ErrorCodes.SnapshotTooOld);
            runCommand(
                db, testCase.command(collName, recipientDoc.rejectReadsBeforeTimestamp), null);
        } else {
            // Untimestamped reads are not rejected after the recipient has applied data past the
            // rejectReadsBeforeTimestamp. Snapshot reads with unspecified atClusterTime should have
            // read timestamp >= rejectReadsBeforeTimestamp so are also not rejected.
            runCommand(db, testCase.command(collName), null);
        }
    });

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, migrationOpts.tenantId);

    nodes.forEach(node => {
        const db = node.getDB(dbName);
        if (testCase.requiresReadTimestamp) {
            runCommand(db, testCase.command(collName, preMigrationTimestamp), null);
            runCommand(
                db, testCase.command(collName, recipientDoc.rejectReadsBeforeTimestamp), null);
        } else {
            runCommand(db, testCase.command(collName), null);
        }
    });
}

const testCases = {
    readWithReadConcernLocal: {
        isSupportedOnSecondaries: true,
        command: function(collName) {
            return {
                find: collName,
                readConcern: {
                    level: "local",
                }
            };
        },
    },
    readWithReadConcernAvailable: {
        isSupportedOnSecondaries: true,
        command: function(collName) {
            return {
                find: collName,
                readConcern: {
                    level: "available",
                }
            };
        },
    },
    readWithReadConcernMajority: {
        isSupportedOnSecondaries: true,
        command: function(collName) {
            return {
                find: collName,
                readConcern: {
                    level: "majority",
                }
            };
        },
    },
    linearizableRead: {
        isSupportedOnSecondaries: false,
        command: function(collName) {
            return {
                find: collName,
                readConcern: {level: "linearizable"},
            };
        }
    },
    snapshotReadWithAtClusterTime: {
        isSupportedOnSecondaries: true,
        requiresReadTimestamp: true,
        command: function(collName, readTimestamp) {
            return {
                find: collName,
                readConcern: {
                    level: "snapshot",
                    atClusterTime: readTimestamp,
                }
            };
        },
    },
    snapshotReadNoAtClusterTime: {
        isSupportedOnSecondaries: true,
        command: function(collName) {
            return {
                find: collName,
                readConcern: {
                    level: "snapshot",
                }
            };
        },
    },
    snapshotReadAtClusterTimeTxn: {
        isSupportedOnSecondaries: false,
        requiresReadTimestamp: true,
        command: function(collName, readTimestamp) {
            return {
                find: collName,
                lsid: {id: UUID()},
                txnNumber: NumberLong(0),
                startTransaction: true,
                autocommit: false,
                readConcern: {level: "snapshot", atClusterTime: readTimestamp}
            };
        }
    },
    snapshotReadNoAtClusterTimeTxn: {
        isSupportedOnSecondaries: false,
        command: function(collName) {
            return {
                find: collName,
                lsid: {id: UUID()},
                txnNumber: NumberLong(0),
                startTransaction: true,
                autocommit: false,
                readConcern: {level: "snapshot"}
            };
        }
    },
};

const testFuncs = {
    afterCloningDone: testRejectAllReadsAfterCloningDone,
    afterReachingBlockTs: testRejectOnlyReadsWithAtClusterTimeLessThanRejectReadsBeforeTimestamp,
    abortBeforeReachingBlockTs:
        testDoNotRejectReadsAfterMigrationAbortedBeforeReachingRejectReadsBeforeTimestamp,
    abortAfterReachingBlockTs:
        testDoNotRejectReadsAfterMigrationAbortedAfterReachingRejectReadsBeforeTimestamp
};

for (const [testName, testFunc] of Object.entries(testFuncs)) {
    for (const [testCaseName, testCase] of Object.entries(testCases)) {
        jsTest.log("Testing " + testName + " with testCase " + testCaseName);
        let tenantId = `${testCaseName}-${testName}`;
        let dbName = `${tenantId}_${kTenantDefinedDbName}`;
        const tenantMigrationTest = new TenantMigrationTest({
            name: jsTestName(),
            quickGarbageCollection: true,
            insertDataForTenant: tenantId,
        });

        // Force the recipient to preserve all snapshot history to ensure that snapshot reads do not
        // fail with SnapshotTooOld due to snapshot being unavailable.
        tenantMigrationTest.getRecipientRst().nodes.forEach(node => {
            configureFailPoint(node, "WTPreserveSnapshotHistoryIndefinitely");
        });

        testFunc({testCase, dbName, collName: kCollName, tenantMigrationTest});
        tenantMigrationTest.stop();
    }
}
})();
