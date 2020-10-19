/**
 * Tests that tenant migrations resume successfully on stepup and restart.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, requires_persistence,
 * incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kMaxSleepTimeMS = 100;
const kConfigDonorsNS = "config.tenantMigrationDonors";
const kTenantId = "testTenantId";

// Set the delay before a donor state doc is garbage collected to be short to speed up the test.
const kGarbageCollectionDelayMS = 30 * 1000;

// Set the TTL monitor to run at a smaller interval to speed up the test.
const kTTLMonitorSleepSecs = 1;

/**
 * If the donor state doc for the migration 'migrationId' exists on the donor (i.e. the donor's
 * primary stepped down or shut down after inserting the doc), asserts that the migration
 * eventually commits.
 */
function assertMigrationCommitsIfDurableStateExists(donorRst, migrationId, tenantId) {
    const donorPrimary = donorRst.getPrimary();
    const configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);
    if (configDonorsColl.count({_id: migrationId}) > 0) {
        TenantMigrationUtil.waitForMigrationToCommit(donorRst.nodes, migrationId, tenantId);
    }
}

/**
 * Runs the donorStartMigration command to start a migration, and interrupts the migration on the
 * donor using the 'interruptFunc', and asserts that migration eventually commits.
 */
function testDonorStartMigrationInterrupt(interruptFunc) {
    const donorRst = new ReplSetTest(
        {nodes: 3, name: "donorRst", nodeOptions: {setParameter: {enableTenantMigrations: true}}});
    const recipientRst = new ReplSetTest({
        nodes: 1,
        name: "recipientRst",
        nodeOptions: {
            setParameter: {
                enableTenantMigrations: true,
                // TODO SERVER-51734: Remove the failpoint
                // 'returnResponseOkForRecipientSyncDataCmd'.
                'failpoint.returnResponseOkForRecipientSyncDataCmd': tojson({mode: 'alwaysOn'})
            }
        }
    });

    donorRst.startSet();
    donorRst.initiate();

    recipientRst.startSet();
    recipientRst.initiate();

    const donorPrimary = donorRst.getPrimary();

    const donorRstArgs = {
        name: donorRst.name,
        nodeHosts: donorRst.nodes.map(node => `127.0.0.1:${node.port}`),
        nodeOptions: donorRst.nodeOptions,
        keyFile: donorRst.keyFile,
        host: donorRst.host,
        waitForKeys: false,
    };

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: recipientRst.getURL(),
        tenantId: kTenantId,
        readPreference: {mode: "primary"},
    };

    let migrationThread = new Thread(
        TenantMigrationUtil.startMigrationRetryOnRetryableErrors, donorRstArgs, migrationOpts);
    migrationThread.start();

    // Wait for to donorStartMigration command to start.
    assert.soon(() => donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"})
                          .inprog.length > 0);

    sleep(Math.random() * kMaxSleepTimeMS);
    interruptFunc(donorRst);

    assert.commandWorked(migrationThread.returnData());
    assertMigrationCommitsIfDurableStateExists(donorRst, migrationId, migrationOpts.tenantId);

    donorRst.stopSet();
    recipientRst.stopSet();
}

/**
 * Starts a migration and waits for it to commit, then runs the donorForgetMigration, and interrupts
 * the donor using the 'interruptFunc', and asserts that the migration state is eventually garbage
 * collected.
 */
function testDonorForgetMigrationInterrupt(interruptFunc) {
    const donorRst = new ReplSetTest({
        nodes: 3,
        name: "donorRst",
        nodeOptions: {
            setParameter: {
                enableTenantMigrations: true,
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: kTTLMonitorSleepSecs,
            }
        }
    });
    const recipientRst = new ReplSetTest({
        nodes: 1,
        name: "recipientRst",
        nodeOptions: {
            setParameter: {
                enableTenantMigrations: true,
                // TODO SERVER-51734: Remove the failpoint
                // 'returnResponseOkForRecipientSyncDataCmd'.
                'failpoint.returnResponseOkForRecipientSyncDataCmd': tojson({mode: 'alwaysOn'}),
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: kTTLMonitorSleepSecs,
            }
        }
    });

    donorRst.startSet();
    donorRst.initiate();

    recipientRst.startSet();
    recipientRst.initiate();

    let donorPrimary = donorRst.getPrimary();

    const donorRstArgs = {
        name: donorRst.name,
        nodeHosts: donorRst.nodes.map(node => `127.0.0.1:${node.port}`),
        nodeOptions: donorRst.nodeOptions,
        keyFile: donorRst.keyFile,
        host: donorRst.host,
        waitForKeys: false,
    };

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: recipientRst.getURL(),
        tenantId: kTenantId,
        readPreference: {mode: "primary"},
    };

    donorPrimary.getCollection(kConfigDonorsNS).createIndex({expireAt: 1}, {expireAfterSeconds: 0});

    assert.commandWorked(TenantMigrationUtil.startMigration(donorPrimary.host, migrationOpts));
    let forgetMigrationThread =
        new Thread(TenantMigrationUtil.forgetMigrationRetryOnRetryableErrors,
                   donorRstArgs,
                   migrationOpts.migrationIdString);
    forgetMigrationThread.start();

    // Wait for to donorForgetMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(
            donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
        return res.inprog[0].expireAt != null;
    });

    sleep(Math.random() * kMaxSleepTimeMS);
    interruptFunc(donorRst);

    donorPrimary = donorRst.getPrimary();
    assert.commandWorkedOrFailedWithCode(
        TenantMigrationUtil.forgetMigration(donorPrimary.host, extractUUIDFromObject(migrationId)),
        ErrorCodes.NoSuchTenantMigration);

    assert.commandWorked(forgetMigrationThread.returnData());
    TenantMigrationUtil.waitForMigrationGarbageCollection(
        donorRst.nodes, migrationId, migrationOpts.tenantId);

    donorRst.stopSet();
    recipientRst.stopSet();
}

(() => {
    jsTest.log("Test that the migration resumes on stepup");
    testDonorStartMigrationInterrupt((donorRst) => {
        // Use a short replSetStepDown seconds to make it more likely for the old primary to
        // step back up.
        assert.commandWorked(donorRst.getPrimary().adminCommand({replSetStepDown: 1, force: true}));
    });
})();

(() => {
    jsTest.log("Test that the migration resumes after restart");
    testDonorStartMigrationInterrupt((donorRst) => {
        donorRst.stopSet(null /* signal */, true /*forRestart */);
        donorRst.startSet({restart: true});
    });
})();

(() => {
    jsTest.log("Test that the donorForgetMigration command can be retried on stepup");
    testDonorForgetMigrationInterrupt((donorRst) => {
        // Use a short replSetStepDown seconds to make it more likely for the old primary to
        // step back up.
        assert.commandWorked(donorRst.getPrimary().adminCommand({replSetStepDown: 1, force: true}));
    });
})();

(() => {
    jsTest.log("Test that the donorForgetMigration command can be retried after restart");
    testDonorForgetMigrationInterrupt((donorRst) => {
        donorRst.stopSet(null /* signal */, true /*forRestart */);
        donorRst.startSet({restart: true});
    });
})();
})();
