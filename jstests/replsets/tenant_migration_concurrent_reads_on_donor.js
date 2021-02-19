/**
 * Tests that the donor
 * - blocks reads with atClusterTime/afterClusterTime >= blockTimestamp that are executed while the
 *   migration is in the blocking state but does not block linearizable reads.
 * - rejects reads with atClusterTime/afterClusterTime >= blockTimestamp reads and linearizable
 *   reads after the migration commits.
 * - does not reject reads with atClusterTime/afterClusterTime >= blockTimestamp and linearizable
 *   reads after the migration aborts.
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

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const kCollName = "testColl";
const kTenantDefinedDbName = "0";

const kMaxTimeMS = 5 * 1000;

/**
 * Asserts that the TenantMigrationAccessBlocker for the given tenant on the given node has the
 * expected statistics.
 */
function checkTenantMigrationAccessBlocker(node, tenantId, {
    numBlockedReads = 0,
    numTenantMigrationCommittedErrors = 0,
    numTenantMigrationAbortedErrors = 0
}) {
    const mtab = TenantMigrationUtil.getTenantMigrationAccessBlocker(node, tenantId);
    if (!mtab) {
        assert.eq(0, numBlockedReads);
        assert.eq(0, numTenantMigrationCommittedErrors);
        assert.eq(0, numTenantMigrationAbortedErrors);
        return;
    }

    assert.eq(mtab.numBlockedReads, numBlockedReads, tojson(mtab));
    assert.eq(mtab.numBlockedWrites, 0, tojson(mtab));
    assert.eq(
        mtab.numTenantMigrationCommittedErrors, numTenantMigrationCommittedErrors, tojson(mtab));
    assert.eq(mtab.numTenantMigrationAbortedErrors, numTenantMigrationAbortedErrors, tojson(mtab));
}

/**
 * To be used to resume a migration that is paused after entering the blocking state. Waits for the
 * number of blocked reads to reach 'targetNumBlockedReads' and unpauses the migration.
 */
function resumeMigrationAfterBlockingRead(host, tenantId, targetNumBlockedReads) {
    load("jstests/libs/fail_point_util.js");
    load("jstests/replsets/libs/tenant_migration_util.js");
    const primary = new Mongo(host);

    assert.soon(() => TenantMigrationUtil.getNumBlockedReads(primary, tenantId) ==
                    targetNumBlockedReads);

    assert.commandWorked(primary.adminCommand(
        {configureFailPoint: "pauseTenantMigrationBeforeLeavingBlockingState", mode: "off"}));
}

function runCommand(db, cmd, expectedError, isTransaction) {
    const res = db.runCommand(cmd);

    if (expectedError) {
        assert.commandFailedWithCode(res, expectedError, tojson(cmd));
        // The 'TransientTransactionError' label is attached only in a scope of a transaction.
        if (isTransaction &&
            (expectedError == ErrorCodes.TenantMigrationAborted ||
             expectedError == ErrorCodes.TenantMigrationCommitted)) {
            assert(res["errorLabels"] != null, "Error labels are absent from " + tojson(res));
            const expectedErrorLabels = ['TransientTransactionError'];
            assert.sameMembers(res["errorLabels"],
                               expectedErrorLabels,
                               "Error labels " + tojson(res["errorLabels"]) +
                                   " are different from expected " + expectedErrorLabels);
        }
    } else {
        assert.commandWorked(res);
    }

    if (cmd.lsid) {
        assert.commandWorked(db.runCommand({killSessions: [cmd.lsid]}));
    }
}

/**
 * Tests that after the migration commits, the donor rejects linearizable reads and reads with
 * atClusterTime/afterClusterTime >= blockTimestamp.
 */
function testRejectReadsAfterMigrationCommitted(testCase, dbName, collName) {
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = donorRst.getPrimary();

    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));
    assert.eq(stateRes.state, TenantMigrationTest.State.kCommitted);

    // Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
    // the oplog on all the secondaries. This is to ensure that snapshot reads on secondaries with
    // unspecified atClusterTime have read timestamp >= commitTimestamp.
    donorRst.awaitLastOpCommitted();

    const donorDoc = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({
        tenantId: tenantId
    });
    const nodes = testCase.isSupportedOnSecondaries ? donorRst.nodes : [donorPrimary];
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        if (testCase.requiresReadTimestamp) {
            runCommand(db,
                       testCase.command(collName, donorDoc.blockTimestamp),
                       ErrorCodes.TenantMigrationCommitted,
                       testCase.isTransaction);
            runCommand(db,
                       testCase.command(collName, donorDoc.commitOrAbortOpTime.ts),
                       ErrorCodes.TenantMigrationCommitted,
                       testCase.isTransaction);
            checkTenantMigrationAccessBlocker(
                node, tenantId, {numTenantMigrationCommittedErrors: 2});
        } else {
            runCommand(db,
                       testCase.command(collName),
                       ErrorCodes.TenantMigrationCommitted,
                       testCase.isTransaction);
            checkTenantMigrationAccessBlocker(
                node, tenantId, {numTenantMigrationCommittedErrors: 1});
        }
    });
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
}

/**
 * Tests that after the migration abort, the donor does not reject linearizable reads or reads with
 * atClusterTime/afterClusterTime >= blockTimestamp.
 */
function testDoNotRejectReadsAfterMigrationAborted(testCase, dbName, collName) {
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = donorRst.getPrimary();

    let abortFp =
        configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");
    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);
    abortFp.off();

    // Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
    // the oplog on all the secondaries. This is to ensure that snapshot reads on secondaries with
    // unspecified atClusterTime have read timestamp >= abortTimestamp.
    donorRst.awaitLastOpCommitted();

    const donorDoc = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({
        tenantId: tenantId
    });
    const nodes = testCase.isSupportedOnSecondaries ? donorRst.nodes : [donorPrimary];
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        if (testCase.requiresReadTimestamp) {
            runCommand(db,
                       testCase.command(collName, donorDoc.blockTimestamp),
                       null,
                       testCase.isTransaction);
            runCommand(db,
                       testCase.command(collName, donorDoc.commitOrAbortOpTime.ts),
                       null,
                       testCase.isTransaction);
            checkTenantMigrationAccessBlocker(node, tenantId, {numTenantMigrationAbortedErrors: 0});
        } else {
            runCommand(db, testCase.command(collName), null, testCase.isTransaction);
            checkTenantMigrationAccessBlocker(node, tenantId, {numTenantMigrationAbortedErrors: 0});
        }
    });
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
}

/**
 * Tests that in the blocking state, the donor blocks reads with atClusterTime/afterClusterTime >=
 * blockTimestamp but does not block linearizable reads.
 */
function testBlockReadsAfterMigrationEnteredBlocking(testCase, dbName, collName) {
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = donorRst.getPrimary();

    let blockingFp =
        configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    // Wait for the migration to enter the blocking state.
    blockingFp.wait();

    // Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
    // the oplog on all secondaries to ensure that snapshot reads on the secondaries with
    // unspecified atClusterTime have read timestamp >= blockTimestamp.
    donorRst.awaitLastOpCommitted();

    const donorDoc = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({
        tenantId: tenantId
    });
    const command = testCase.requiresReadTimestamp
        ? testCase.command(collName, donorDoc.blockTimestamp)
        : testCase.command(collName);
    command.maxTimeMS = kMaxTimeMS;

    const nodes = testCase.isSupportedOnSecondaries ? donorRst.nodes : [donorPrimary];
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        const shouldBlock = !testCase.isLinearizableRead;
        runCommand(
            db, command, shouldBlock ? ErrorCodes.MaxTimeMSExpired : null, testCase.isTransaction);
        checkTenantMigrationAccessBlocker(node, tenantId, {numBlockedReads: shouldBlock ? 1 : 0});
    });

    blockingFp.off();
    const stateRes =
        assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kCommitted);
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
}

/**
 * Tests that the donor rejects the blocked reads (reads with atClusterTime/afterClusterTime >=
 * blockingTimestamp) once the migration commits.
 */
function testRejectBlockedReadsAfterMigrationCommitted(testCase, dbName, collName) {
    if (testCase.isLinearizableRead) {
        // Linearizable reads are not blocked.
        return;
    }

    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = donorRst.getPrimary();

    let blockingFp =
        configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    let resumeMigrationThread =
        new Thread(resumeMigrationAfterBlockingRead, donorPrimary.host, tenantId, 1);

    // Run the commands after the migration enters the blocking state.
    resumeMigrationThread.start();
    blockingFp.wait();

    // Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
    // the oplog on all secondaries to ensure that snapshot reads on the secondaries with
    // unspecified atClusterTime have read timestamp >= blockTimestamp.
    donorRst.awaitLastOpCommitted();

    const donorDoc = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({
        tenantId: tenantId
    });
    const command = testCase.requiresReadTimestamp
        ? testCase.command(collName, donorDoc.blockTimestamp)
        : testCase.command(collName);
    const nodes = testCase.isSupportedOnSecondaries ? donorRst.nodes : [donorPrimary];

    // The migration should unpause and commit after the read is blocked. Verify that the read
    // is rejected.
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        runCommand(db, command, ErrorCodes.TenantMigrationCommitted, testCase.isTransaction);
    });

    const shouldBlock = !testCase.isLinearizableRead;
    checkTenantMigrationAccessBlocker(
        donorPrimary,
        tenantId,
        {numBlockedReads: shouldBlock ? 1 : 0, numTenantMigrationCommittedErrors: 1});

    // Verify that the migration succeeded.
    resumeMigrationThread.join();
    const stateRes =
        assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kCommitted);
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
}

/**
 * Tests that the donor unblocks blocked reads (reads with atClusterTime/afterClusterTime >=
 * blockingTimestamp) once the migration aborts.
 */
function testUnblockBlockedReadsAfterMigrationAborted(testCase, dbName, collName) {
    if (testCase.isLinearizableRead) {
        // Linearizable reads are not blocked.
        return;
    }

    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = donorRst.getPrimary();

    let blockingFp =
        configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");
    let abortFp =
        configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    let resumeMigrationThread =
        new Thread(resumeMigrationAfterBlockingRead, donorPrimary.host, tenantId, 1);

    // Run the commands after the migration enters the blocking state.
    resumeMigrationThread.start();
    blockingFp.wait();

    // Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
    // the oplog on all secondaries to ensure that snapshot reads on the secondaries with
    // unspecified atClusterTime have read timestamp >= blockTimestamp.
    donorRst.awaitLastOpCommitted();

    const donorDoc = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({
        tenantId: tenantId
    });
    const command = testCase.requiresReadTimestamp
        ? testCase.command(collName, donorDoc.blockTimestamp)
        : testCase.command(collName);
    const nodes = testCase.isSupportedOnSecondaries ? donorRst.nodes : [donorPrimary];

    // The migration should unpause and abort after the read is blocked. Verify that the read
    // unblocks.
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        runCommand(db, command, null, testCase.isTransaction);
    });

    const shouldBlock = !testCase.isLinearizableRead;
    checkTenantMigrationAccessBlocker(donorPrimary, tenantId, {
        numBlockedReads: shouldBlock ? 1 : 0,
        // Reads just get unblocked if the migration aborts.
        numTenantMigrationAbortedErrors: 0
    });

    // Verify that the migration failed due to the simulated error.
    resumeMigrationThread.join();
    abortFp.off();
    const stateRes =
        assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
}

const testCases = {
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
    snapshotReadWithoutAtClusterTime: {
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
    snapshotReadWithAtClusterTimeInTxn: {
        isSupportedOnSecondaries: false,
        requiresReadTimestamp: true,
        isTransaction: true,
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
    snapshotReadWithoutAtClusterTimeInTxn: {
        isSupportedOnSecondaries: false,
        isTransaction: true,
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
    readWithAfterClusterTime: {
        isSupportedOnSecondaries: true,
        requiresReadTimestamp: true,
        command: function(collName, readTimestamp) {
            return {
                find: collName,
                readConcern: {
                    afterClusterTime: readTimestamp,
                }
            };
        },
    },
    readWithAfterClusterTimeInTxn: {
        isSupportedOnSecondaries: false,
        requiresReadTimestamp: true,
        isTransaction: true,
        command: function(collName, readTimestamp) {
            return {
                find: collName,
                lsid: {id: UUID()},
                txnNumber: NumberLong(0),
                startTransaction: true,
                autocommit: false,
                readConcern: {
                    afterClusterTime: readTimestamp,
                }
            };
        },
    },
    linearizableRead: {
        isSupportedOnSecondaries: false,
        isLinearizableRead: true,
        command: function(collName) {
            return {
                find: collName,
                readConcern: {level: "linearizable"},
            };
        }
    }
};

const testFuncs = {
    inCommitted: testRejectReadsAfterMigrationCommitted,
    inAborted: testDoNotRejectReadsAfterMigrationAborted,
    inBlocking: testBlockReadsAfterMigrationEnteredBlocking,
    inBlockingThenCommitted: testRejectBlockedReadsAfterMigrationCommitted,
    inBlockingThenAborted: testUnblockBlockedReadsAfterMigrationAborted,
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
