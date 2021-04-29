/**
 * Helper functions for running tests related to sync source selection during a tenant migration.
 */

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load('jstests/replsets/rslib.js');

/**
 * Starts up a tenant migration with 'secondary' read preference, and ensures that both donor
 * secondaries are not eligible sync sources.
 *
 * When this function returns, the recipient primary should be hanging during sync source selection.
 * We expect 'donorSecondary' to be shut down and 'delayedSecondary' to be behind the
 * 'startApplyingDonorOpTime' stored in the recipient state document. As a result, neither nodes are
 * eligible sync sources for the migration.
 */
const setUpMigrationSyncSourceTest = function() {
    const donorRst = new ReplSetTest({
        name: `${jsTestName()}_donor`,
        nodes: 3,
        settings: {chainingAllowed: false},
        nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().donor, {
            setParameter: {
                tenantMigrationExcludeDonorHostTimeoutMS: 30 * 1000,
                // Allow non-timestamped reads on donor after migration completes for testing.
                'failpoint.tenantMigrationDonorAllowsNonTimestampedReads':
                    tojson({mode: 'alwaysOn'}),
            }
        }),
    });
    donorRst.startSet();
    donorRst.initiateWithHighElectionTimeout();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        return {};
    }
    const tenantId = "testTenantId";
    const tenantDB = tenantMigrationTest.tenantDB(tenantId, "DB");
    const collName = "testColl";

    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const delayedSecondary = donorRst.getSecondaries()[0];
    const donorSecondary = donorRst.getSecondaries()[1];
    // The default WC is majority and stopServerReplication will prevent satisfying any majority
    // writes.
    assert.commandWorked(donorPrimary.adminCommand(
        {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
    donorRst.awaitReplication();

    const recipientRst = tenantMigrationTest.getRecipientRst();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    const newRecipientPrimary = recipientRst.getSecondary();

    tenantMigrationTest.insertDonorDB(tenantDB, collName);

    const hangDonorBeforeEnteringDataSync = configureFailPoint(
        donorPrimary, "pauseTenantMigrationBeforeLeavingAbortingIndexBuildsState");

    const hangRecipientPrimaryAfterCreatingRSM =
        configureFailPoint(recipientPrimary, 'hangAfterCreatingRSM');
    const hangRecipientPrimaryAfterCreatingConnections =
        configureFailPoint(recipientPrimary,
                           'fpAfterStartingOplogFetcherMigrationRecipientInstance',
                           {action: "hang"});

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
        // The recipient primary can only choose secondaries as sync sources.
        readPreference: {mode: 'secondary'},
    };

    jsTestLog("Starting the tenant migration");
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    // Stop replicating on one of the secondaries so that its majority OpTime will be behind the
    // recipient's 'startApplyingDonorOpTime'. Do this immediately before the write to enter the
    // data sync state, so external keys will already have replicated to every donor node.
    hangDonorBeforeEnteringDataSync.wait();
    stopServerReplication(delayedSecondary);
    hangDonorBeforeEnteringDataSync.off();

    hangRecipientPrimaryAfterCreatingRSM.wait();

    awaitRSClientHosts(recipientPrimary, donorSecondary, {ok: true, secondary: true});
    awaitRSClientHosts(recipientPrimary, delayedSecondary, {ok: true, secondary: true});

    // Turn on the 'waitInHello' failpoint. This will cause the delayed secondary to cease sending
    // hello responses and the RSM should mark the node as down. This is necessary so that the
    // delayed secondary is not chosen as the sync source here, since we want the
    // 'startApplyingDonorOpTime' to be set to the most advanced majority OpTime.
    jsTestLog(
        "Turning on waitInHello failpoint. Delayed donor secondary should stop sending hello responses.");
    const helloFailpoint = configureFailPoint(delayedSecondary, "waitInHello");
    awaitRSClientHosts(recipientPrimary, delayedSecondary, {ok: false});

    hangRecipientPrimaryAfterCreatingRSM.off();
    hangRecipientPrimaryAfterCreatingConnections.wait();

    let res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
    let currOp = res.inprog[0];
    // The migration should not be complete.
    assert.eq(currOp.migrationCompleted, false, tojson(res));
    assert.eq(currOp.dataSyncCompleted, false, tojson(res));
    // The sync source can only be 'donorSecondary'.
    assert.eq(donorSecondary.host, currOp.donorSyncSource, tojson(res));

    helloFailpoint.off();

    const hangNewRecipientPrimaryAfterCreatingRSM =
        configureFailPoint(newRecipientPrimary, 'hangAfterCreatingRSM');
    const hangNewRecipientPrimaryAfterCreatingConnections =
        configureFailPoint(newRecipientPrimary,
                           'fpAfterRetrievingStartOpTimesMigrationRecipientInstance',
                           {action: "hang"});

    // Step up a new primary so that the tenant migration restarts on the new primary, with the
    // 'startApplyingDonorOpTime' field already set in the state doc.
    jsTestLog("Stepping up the recipient secondary");
    recipientRst.awaitLastOpCommitted();
    recipientRst.stepUp(newRecipientPrimary);
    assert.eq(newRecipientPrimary, recipientRst.getPrimary());

    jsTestLog("Stopping the non-lagged secondary");
    donorRst.stop(donorSecondary);

    // Wait for the new primary to see the state of each donor node. 'donorSecondary' should return
    // '{ok: false}' since it has been shut down.
    hangNewRecipientPrimaryAfterCreatingRSM.wait();
    awaitRSClientHosts(newRecipientPrimary, donorPrimary, {ok: true, ismaster: true});
    awaitRSClientHosts(newRecipientPrimary, delayedSecondary, {ok: true, secondary: true});
    awaitRSClientHosts(newRecipientPrimary, donorSecondary, {ok: false});

    jsTestLog("Releasing failpoints");
    hangNewRecipientPrimaryAfterCreatingRSM.off();
    hangRecipientPrimaryAfterCreatingConnections.off();

    res = newRecipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
    currOp = res.inprog[0];
    // The migration should not be complete and there should be no sync source stored, since the new
    // recipient primary does not have a valid sync source to choose from.
    assert.eq(currOp.migrationCompleted, false, tojson(res));
    assert.eq(currOp.dataSyncCompleted, false, tojson(res));
    assert(!currOp.donorSyncSource, tojson(res));

    return {
        tenantMigrationTest,
        migrationOpts,
        donorSecondary,
        delayedSecondary,
        hangAfterCreatingConnections: hangNewRecipientPrimaryAfterCreatingConnections
    };
};
