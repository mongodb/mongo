/**
 * Tests that the recipient
 * - rejects all reads between when cloning is done and when the returnAfterReachingTimestamp
 *   (blockTimestamp) is received and reached.
 * - rejects only reads with atClusterTime < returnAfterReachingTimestamp (blockTimestamp) after
 *   returnAfterReachingTimestamp is reached.
 * - does not reject any reads after the migration aborts.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/replsets/rslib.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

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
function testRejectAllReadsAfterCloningDone(testCase, dbName, collName) {
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    const donorRst = tenantMigrationTest.getDonorRst();
    const recipientRst = tenantMigrationTest.getRecipientRst();
    const recipientPrimary = recipientRst.getPrimary();

    let clonerDoneFp =
        configureFailPoint(recipientPrimary, "fpAfterCollectionClonerDone", {action: "hang"});

    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);
    const runMigrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
    runMigrationThread.start();
    clonerDoneFp.wait();

    const nodes = testCase.isSupportedOnSecondaries ? recipientRst.nodes : [recipientPrimary];
    nodes.forEach(node => {
        const command = testCase.requiresReadTimestamp
            ? testCase.command(collName, getLastOpTime(node).ts)
            : testCase.command(collName);
        const db = node.getDB(dbName);
        runCommand(db, command, ErrorCodes.SnapshotTooOld);
    });

    clonerDoneFp.off();
    const stateRes = assert.commandWorked(runMigrationThread.returnData());
    assert.eq(stateRes.state, TenantMigrationTest.DonorState.kCommitted);
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
}

/**
 * Tests that after the recipient has reached the returnAfterReachingTimestamp (blockTimestamp) and
 * after the migration commits, it only rejects reads with atClusterTime < blockTimestamp.
 */
function testRejectOnlyReadsWithAtClusterTimeLessThanBlockTimestamp(testCase, dbName, collName) {
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

    // Select a read timestamp < blockTimestamp.
    const preMigrationTimestamp = getLastOpTime(donorPrimary).ts;

    let waitForRejectReadsBeforeTsFp = configureFailPoint(
        recipientPrimary, "fpAfterWaitForRejectReadsBeforeTimestamp", {action: "hang"});

    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);
    const runMigrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
    runMigrationThread.start();
    waitForRejectReadsBeforeTsFp.wait();

    const donorDoc = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({
        tenantId: tenantId
    });
    assert.lt(preMigrationTimestamp, donorDoc.blockTimestamp);

    const nodes = testCase.isSupportedOnSecondaries ? recipientRst.nodes : [recipientPrimary];
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        if (testCase.requiresReadTimestamp) {
            runCommand(
                db, testCase.command(collName, preMigrationTimestamp), ErrorCodes.SnapshotTooOld);
            runCommand(db, testCase.command(collName, donorDoc.blockTimestamp), null);
        } else {
            // Untimestamped reads are not rejected after the recipient has applied data past the
            // blockTimestamp. Snapshot reads with unspecified atClusterTime should have read
            // timestamp >= blockTimestamp so are also not rejected.
            runCommand(db, testCase.command(collName), null);
        }
    });

    waitForRejectReadsBeforeTsFp.off();
    const stateRes = assert.commandWorked(runMigrationThread.returnData());
    assert.eq(stateRes.state, TenantMigrationTest.DonorState.kCommitted);

    nodes.forEach(node => {
        const db = node.getDB(dbName);
        if (testCase.requiresReadTimestamp) {
            runCommand(
                db, testCase.command(collName, preMigrationTimestamp), ErrorCodes.SnapshotTooOld);
            runCommand(db, testCase.command(collName, donorDoc.blockTimestamp), null);
        } else {
            // Untimestamped reads are not rejected after the recipient has committed. Snapshot
            // reads with unspecified atClusterTime should have read timestamp >= blockTimestamp so
            // are also not rejected.
            runCommand(db, testCase.command(collName), null);
        }
    });

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
}

/**
 * Tests that after the migration aborts before the recipient receives the
 * returnAfterReachingTimestamp (blockTimestamp), the recipient stops rejecting reads.
 */
function testDoNotRejectReadsAfterMigrationAbortedBeforeReachingBlockTimestamp(
    testCase, dbName, collName) {
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
    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));
    assert.eq(stateRes.state, TenantMigrationTest.DonorState.kAborted);
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
 * Tests that after the migration aborts after the recipient has reached the
 * returnAfterReachingTimestamp (blockTimestamp), the recipient stops rejecting reads.
 */
function testDoNotRejectReadsAfterMigrationAbortedAfterReachingBlockTimestamp(
    testCase, dbName, collName) {
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = donorRst.getPrimary();
    const recipientRst = tenantMigrationTest.getRecipientRst();
    const recipientPrimary = recipientRst.getPrimary();

    // Force the donor to abort the migration right after the recipient responds to the second
    // recipientSyncData (i.e. after it has reached the returnAfterReachingTimestamp).
    let abortFp =
        configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");
    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));
    assert.eq(stateRes.state, TenantMigrationTest.DonorState.kAborted);
    abortFp.off();

    const donorDoc = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({
        tenantId: tenantId
    });

    const nodes = testCase.isSupportedOnSecondaries ? recipientRst.nodes : [recipientPrimary];
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        if (testCase.requiresReadTimestamp) {
            runCommand(db, testCase.command(collName, donorDoc.blockTimestamp), null);
        } else {
            runCommand(db, testCase.command(collName), null);
        }
    });
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
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
    snapshotReadWithAtClusterTimeTxn: {
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

// Force the recipient to preserve all snapshot history to ensure that snapshot reads do not fail
// with SnapshotTooOld due to snapshot being unavailable.
const recipientRst = tenantMigrationTest.getRecipientRst();
recipientRst.nodes.forEach(node => {
    configureFailPoint(node, "WTPreserveSnapshotHistoryIndefinitely");
});

const testFuncs = {
    afterCloningDone: testRejectAllReadsAfterCloningDone,
    afterReachingBlockTs: testRejectOnlyReadsWithAtClusterTimeLessThanBlockTimestamp,
    abortBeforeReachingBlockTs:
        testDoNotRejectReadsAfterMigrationAbortedBeforeReachingBlockTimestamp,
    abortAfterReachingBlockTs: testDoNotRejectReadsAfterMigrationAbortedAfterReachingBlockTimestamp
};

for (const [testName, testFunc] of Object.entries(testFuncs)) {
    for (const [testCaseName, testCase] of Object.entries(testCases)) {
        jsTest.log("Testing " + testName + " with testCase " + testCaseName);
        let dbName = testCaseName + "-" + testName + "_" + kTenantDefinedDbName;
        testFunc(testCase, dbName, kCollName);
    }
}

tenantMigrationTest.stop();
})();
