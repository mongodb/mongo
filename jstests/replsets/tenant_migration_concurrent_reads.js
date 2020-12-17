/**
 * Tests that that the donor
 * - blocks clusterTime reads that are executed while the migration is in the blocking state but
 *   does not block linearizable reads.
 * - rejects (blocked) clusterTime reads and linearizable reads after the migration commits.
 * - does not reject (blocked) clusterTime reads and linearizable reads after the migration aborts.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const kCollName = "testColl";
const kTenantDefinedDbName = "0";

const kMaxTimeMS = 5 * 1000;

/**
 * To be used to resume a migration that is paused after entering the blocking state. Waits for the
 * number of blocked reads to reach 'targetBlockedReads' and unpauses the migration.
 */
function resumeMigrationAfterBlockingRead(host, targetBlockedReads) {
    load("jstests/libs/fail_point_util.js");
    const primary = new Mongo(host);

    assert.commandWorked(primary.adminCommand({
        waitForFailPoint: "tenantMigrationBlockRead",
        timesEntered: targetBlockedReads,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    assert.commandWorked(primary.adminCommand(
        {configureFailPoint: "pauseTenantMigrationAfterBlockingStarts", mode: "off"}));
}

function runCommand(db, cmd, expectedError, isTransaction) {
    const res = db.runCommand(cmd);

    if (expectedError) {
        assert.commandFailedWithCode(res, expectedError);
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
 * Tests that the donor rejects causal reads after the migration commits.
 */
function testReadIsRejectedIfSentAfterMigrationHasCommitted(testCase, dbName, collName) {
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
    // the oplog on all the secondaries. This is to ensure that the write to put the migration into
    // "committed" is majority-committed and that snapshot reads on the secondaries with unspecified
    // atClusterTime have read timestamp >= commitTimestamp.
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
        } else {
            runCommand(db,
                       testCase.command(collName),
                       ErrorCodes.TenantMigrationCommitted,
                       testCase.isTransaction);
        }
    });
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
}

/**
 * Tests that the donor does not reject reads after the migration aborts.
 */
function testReadIsAcceptedIfSentAfterMigrationHasAborted(testCase, dbName, collName) {
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = donorRst.getPrimary();

    let abortFp = configureFailPoint(donorPrimary, "abortTenantMigrationAfterBlockingStarts");
    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);
    abortFp.off();

    // Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
    // the oplog on all the secondaries. This is to ensure that the write to put the migration into
    // "aborted" is majority-committed and that snapshot reads on the secondaries with unspecified
    // atClusterTime have read timestamp >= abortTimestamp.
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
        } else {
            runCommand(db, testCase.command(collName), null, testCase.isTransaction);
        }
    });
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
}

/**
 * Tests that the donor blocks clusterTime reads in the blocking state with readTimestamp >=
 * blockingTimestamp but does not block linearizable reads.
 */
function testReadBlocksIfMigrationIsInBlocking(testCase, dbName, collName) {
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = donorRst.getPrimary();

    let blockingFp = configureFailPoint(donorPrimary, "pauseTenantMigrationAfterBlockingStarts");
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
        runCommand(db,
                   command,
                   testCase.isLinearizableRead ? null : ErrorCodes.MaxTimeMSExpired,
                   testCase.isTransaction);
    });

    blockingFp.off();
    const stateRes =
        assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kCommitted);
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
}

/**
 * Tests that the donor blocks clusterTime reads in the blocking state with readTimestamp >=
 * blockingTimestamp, and unblocks the reads once the migration aborts.
 */
function testBlockedReadGetsUnblockedAndRejectedIfMigrationCommits(testCase, dbName, collName) {
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

    let blockingFp = configureFailPoint(donorPrimary, "pauseTenantMigrationAfterBlockingStarts");
    const targetBlockedReads =
        assert
            .commandWorked(donorPrimary.adminCommand(
                {configureFailPoint: "tenantMigrationBlockRead", mode: "alwaysOn"}))
            .count +
        1;

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    let resumeMigrationThread =
        new Thread(resumeMigrationAfterBlockingRead, donorPrimary.host, targetBlockedReads);

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

    // Verify that the migration succeeded.
    resumeMigrationThread.join();
    const stateRes =
        assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kCommitted);
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
}

/**
 * Tests that the donor blocks clusterTime reads in the blocking state with readTimestamp >=
 * blockingTimestamp, and rejects the reads once the migration commits.
 */
function testBlockedReadGetsUnblockedAndSucceedsIfMigrationAborts(testCase, dbName, collName) {
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

    let blockingFp = configureFailPoint(donorPrimary, "pauseTenantMigrationAfterBlockingStarts");
    let abortFp = configureFailPoint(donorPrimary, "abortTenantMigrationAfterBlockingStarts");
    const targetBlockedReads =
        assert
            .commandWorked(donorPrimary.adminCommand(
                {configureFailPoint: "tenantMigrationBlockRead", mode: "alwaysOn"}))
            .count +
        1;

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    let resumeMigrationThread =
        new Thread(resumeMigrationAfterBlockingRead, donorPrimary.host, targetBlockedReads);

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
    inCommitted: testReadIsRejectedIfSentAfterMigrationHasCommitted,
    inAborted: testReadIsAcceptedIfSentAfterMigrationHasAborted,
    inBlocking: testReadBlocksIfMigrationIsInBlocking,
    inBlockingThenCommitted: testBlockedReadGetsUnblockedAndRejectedIfMigrationCommits,
    inBlockingThenAborted: testBlockedReadGetsUnblockedAndSucceedsIfMigrationAborts,
};

for (const [testName, testFunc] of Object.entries(testFuncs)) {
    for (const [testCaseName, testCase] of Object.entries(testCases)) {
        let dbName = testCaseName + "-" + testName + "_" + kTenantDefinedDbName;
        testFunc(testCase, dbName, kCollName);
    }
}

tenantMigrationTest.stop();
})();
