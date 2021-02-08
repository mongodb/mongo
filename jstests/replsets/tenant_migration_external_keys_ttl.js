/**
 * Tests that tenant migrations correctly set the TTL values for keys in the
 * config.external_validation_keys collection.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/libs/parallelTester.js");

const kExternalKeysTTLIndexName = "ExternalKeysTTLIndex";
const kTenantIdPrefix = "testTenantId";
const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();
const ttlMonitorOptions = {
    ttlMonitorSleepSecs: 1
};

let counter = 0;
let makeTenantId = function() {
    return kTenantIdPrefix + "_" + counter++;
};

function waitForExternalKeysTTLIndex(conn) {
    assert.soon(() => {
        const indexSpecs = conn.getCollection(TenantMigrationUtil.kExternalKeysNs).getIndexSpecs();
        const hasIndex = indexSpecs.some(indexSpec => {
            return indexSpec.name === kExternalKeysTTLIndexName &&
                indexSpec.key.ttlExpiresAt === 1 && indexSpec.expireAfterSeconds === 0;
        });

        if (hasIndex) {
            return true;
        }
        jsTestLog("Waiting for external keys index to be created, current indexes: " +
                  tojson(indexSpecs));
    });
}

function waitForExternalKeysToBeDeleted(conn, migrationId) {
    assert.soonNoExcept(() => {
        const externalKeys = TenantMigrationUtil.getExternalKeys(conn, migrationId);
        assert.eq(0, externalKeys.length, tojson(externalKeys));
        return true;
    });
}

function verifyExternalKeys(conn, {migrationId, expectTTLValue}) {
    const externalKeys =
        conn.getCollection(TenantMigrationUtil.kExternalKeysNs).find({migrationId}).toArray();
    assert.gt(externalKeys.length, 0);

    externalKeys.forEach(key => {
        assert.eq(expectTTLValue, key.hasOwnProperty("ttlExpiresAt"), tojson(key));
    });
}

function setTenantMigrationExpirationParams(conn, stateDocParam, externalKeysParam) {
    const origStateDocParam = assert.commandWorked(
        conn.adminCommand({getParameter: 1, tenantMigrationGarbageCollectionDelayMS: 1}));
    const origExternalKeysParam = assert.commandWorked(
        conn.adminCommand({getParameter: 1, tenantMigrationExternalKeysRemovalBufferSecs: 1}));

    assert.commandWorked(conn.adminCommand(
        {setParameter: 1, tenantMigrationGarbageCollectionDelayMS: stateDocParam}));
    assert.commandWorked(conn.adminCommand(
        {setParameter: 1, tenantMigrationExternalKeysRemovalBufferSecs: externalKeysParam}));

    return [
        origStateDocParam.tenantMigrationGarbageCollectionDelayMS,
        origExternalKeysParam.tenantMigrationExternalKeysRemovalBufferSecs
    ];
}

function makeTestParams() {
    const tenantId = makeTenantId();
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
    };
    return [tenantId, migrationId, migrationOpts];
}

//
// Tests with no failovers.
//

(() => {
    const tmt = new TenantMigrationTest(
        {name: jsTestName(), sharedOptions: {setParameter: ttlMonitorOptions}});

    if (!tmt.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        tmt.stop();
        return;
    }

    // Verify the external keys TTL index is created on both replica sets on stepup.
    waitForExternalKeysTTLIndex(tmt.getDonorPrimary());
    waitForExternalKeysTTLIndex(tmt.getRecipientPrimary());

    jsTestLog("Basic case with multiple migrations");
    {
        const [tenantId, migrationId, migrationOpts] = makeTestParams();

        assert.commandWorked(tmt.runMigration(migrationOpts,
                                              false /* retryOnRetryableErrors */,
                                              false /* automaticForgetMigration */));

        // The keys should have been created without a TTL deadline.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: false});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: false});

        // Run another migration to verify key expiration is only set for a specific migration's
        // keys.
        const otherMigrationId = UUID();
        const otherMigrationOpts = {
            migrationIdString: extractUUIDFromObject(otherMigrationId),
            tenantId: makeTenantId(),
        };

        assert.commandWorked(tmt.runMigration(otherMigrationOpts,
                                              false /* retryOnRetryableErrors */,
                                              false /* automaticForgetMigration */));

        // The keys should have been created without a TTL deadline.
        verifyExternalKeys(tmt.getDonorPrimary(),
                           {migrationId: otherMigrationId, expectTTLValue: false});
        verifyExternalKeys(tmt.getRecipientPrimary(),
                           {migrationId: otherMigrationId, expectTTLValue: false});

        assert.commandWorked(tmt.forgetMigration(migrationOpts.migrationIdString));

        // After running donorForgetMigration, the TTL value should be updated. The default TTL
        // buffer is 1 day so the keys will not have been deleted.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: true});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: true});

        // The keys for the other migration should not have been affected.
        verifyExternalKeys(tmt.getDonorPrimary(),
                           {migrationId: otherMigrationId, expectTTLValue: false});
        verifyExternalKeys(tmt.getRecipientPrimary(),
                           {migrationId: otherMigrationId, expectTTLValue: false});
    }

    jsTestLog("Verify the TTL value is respected and keys are eventually reaped");
    {
        const [tenantId, migrationId, migrationOpts] = makeTestParams();

        const lowerExternalKeysBufferSecs = 5;
        const lowerStateDocExpirationMS = 500;
        const [origDonorStateDocExpirationParam, origDonorKeysExpirationParam] =
            setTenantMigrationExpirationParams(
                tmt.getDonorPrimary(), lowerStateDocExpirationMS, lowerExternalKeysBufferSecs);
        const [origRecipientStateDocExpirationParam, origRecipientKeysExpirationParam] =
            setTenantMigrationExpirationParams(
                tmt.getRecipientPrimary(), lowerStateDocExpirationMS, lowerExternalKeysBufferSecs);

        // Verify the default value of the buffer.
        assert.eq(origDonorKeysExpirationParam, 60 * 60 * 24);      // 1 day.
        assert.eq(origRecipientKeysExpirationParam, 60 * 60 * 24);  // 1 day.

        assert.commandWorked(tmt.runMigration(migrationOpts,
                                              false /* retryOnRetryableErrors */,
                                              false /* automaticForgetMigration */));

        // The keys should have been created without a TTL deadline.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: false});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: false});

        assert.commandWorked(tmt.forgetMigration(migrationOpts.migrationIdString));

        // The keys won't be deleted until the buffer expires, so sleep to avoid wasted work.
        sleep((lowerExternalKeysBufferSecs * 1000) + lowerStateDocExpirationMS + 500);

        // Wait for the keys to be deleted on both replica sets.
        waitForExternalKeysToBeDeleted(tmt.getDonorPrimary(), migrationId);
        waitForExternalKeysToBeDeleted(tmt.getRecipientPrimary(), migrationId);

        // Restore the original timeouts
        setTenantMigrationExpirationParams(
            tmt.getDonorPrimary(), origDonorStateDocExpirationParam, origDonorKeysExpirationParam);
        setTenantMigrationExpirationParams(tmt.getRecipientPrimary(),
                                           origRecipientStateDocExpirationParam,
                                           origRecipientKeysExpirationParam);
    }

    tmt.stop();
})();

//
// Tests with failovers
//

(() => {
    const donorRst = new ReplSetTest({
        nodes: 3,
        name: "donorRst",
        nodeOptions: Object.assign(migrationX509Options.donor, {setParameter: ttlMonitorOptions})
    });
    donorRst.startSet();
    donorRst.initiate();

    const recipientRst = new ReplSetTest({
        nodes: 3,
        name: "recipientRst",
        nodeOptions:
            Object.assign(migrationX509Options.recipient, {setParameter: ttlMonitorOptions})
    });
    recipientRst.startSet();
    recipientRst.initiate();

    const tmt = new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
    if (!tmt.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        recipientRst.stopSet();
        tmt.stop();
        return;
    }

    jsTestLog("Donor failover before receiving forgetMigration");
    {
        const [tenantId, migrationId, migrationOpts] = makeTestParams();
        const donorPrimary = tmt.getDonorPrimary();
        const fp =
            configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");

        assert.commandWorked(tmt.startMigration(migrationOpts));
        fp.wait();

        assert.commandWorked(
            donorPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(donorPrimary.adminCommand({replSetFreeze: 0}));
        fp.off();

        assert.commandWorked(
            tmt.waitForMigrationToComplete(migrationOpts, true /* retryOnRetryableErrors */));

        // The keys should have been created without a TTL deadline.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: false});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: false});

        assert.commandWorked(tmt.forgetMigration(migrationOpts.migrationIdString));

        // After running donorForgetMigration, the TTL value should be updated. The default TTL
        // buffer is 1 day so the keys will not have been deleted.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: true});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: true});
    }

    jsTestLog("Recipient failover before receiving forgetMigration");
    {
        const [tenantId, migrationId, migrationOpts] = makeTestParams();
        const recipientPrimary = tmt.getRecipientPrimary();
        const fp = configureFailPoint(recipientPrimary,
                                      "fpAfterConnectingTenantMigrationRecipientInstance",
                                      {action: "hang"});

        assert.commandWorked(tmt.startMigration(migrationOpts));
        fp.wait();

        assert.commandWorked(recipientPrimary.adminCommand(
            {replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(recipientPrimary.adminCommand({replSetFreeze: 0}));
        fp.off();

        assert.commandWorked(
            tmt.waitForMigrationToComplete(migrationOpts, true /* retryOnRetryableErrors */));

        // The keys should have been created without a TTL deadline.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: false});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: false});

        assert.commandWorked(tmt.forgetMigration(migrationOpts.migrationIdString));

        // After running donorForgetMigration, the TTL value should be updated. The default TTL
        // buffer is 1 day so the keys will not have been deleted.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: true});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: true});
    }

    jsTestLog(
        "Donor failover after receiving forgetMigration before marking keys garbage collectable");
    {
        const [tenantId, migrationId, migrationOpts] = makeTestParams();
        const donorPrimary = tmt.getDonorPrimary();

        assert.commandWorked(tmt.startMigration(migrationOpts));
        assert.commandWorked(
            tmt.waitForMigrationToComplete(migrationOpts, true /* retryOnRetryableErrors */));

        // The keys should have been created without a TTL deadline.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: false});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: false});

        const fp = configureFailPoint(
            donorPrimary, "pauseTenantMigrationBeforeMarkingExternalKeysGarbageCollectable");
        const forgetMigrationThread = new Thread(TenantMigrationUtil.forgetMigrationAsync,
                                                 migrationOpts.migrationIdString,
                                                 TenantMigrationUtil.createRstArgs(donorRst),
                                                 true);
        forgetMigrationThread.start();
        fp.wait();

        assert.commandWorked(
            donorPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(donorPrimary.adminCommand({replSetFreeze: 0}));
        fp.off();

        assert.commandWorked(forgetMigrationThread.returnData());

        // After running donorForgetMigration, the TTL value should be updated. The default TTL
        // buffer is 1 day so the keys will not have been deleted.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: true});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: true});
    }

    jsTestLog(
        "Recipient failover after receiving forgetMigration before marking keys garbage collectable");
    {
        const [tenantId, migrationId, migrationOpts] = makeTestParams();
        const recipientPrimary = tmt.getRecipientPrimary();

        assert.commandWorked(tmt.startMigration(migrationOpts));
        assert.commandWorked(
            tmt.waitForMigrationToComplete(migrationOpts, true /* retryOnRetryableErrors */));

        // The keys should have been created without a TTL deadline.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: false});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: false});

        const fp = configureFailPoint(
            recipientPrimary, "pauseTenantMigrationBeforeMarkingExternalKeysGarbageCollectable");
        const forgetMigrationThread = new Thread(TenantMigrationUtil.forgetMigrationAsync,
                                                 migrationOpts.migrationIdString,
                                                 TenantMigrationUtil.createRstArgs(donorRst),
                                                 true);
        forgetMigrationThread.start();
        fp.wait();

        assert.commandWorked(recipientPrimary.adminCommand(
            {replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(recipientPrimary.adminCommand({replSetFreeze: 0}));
        fp.off();

        assert.commandWorked(forgetMigrationThread.returnData());

        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: true});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: true});
    }

    // The next two cases expect the external keys to expire, so lower the expiration timeouts.
    const lowerExternalKeysBufferSecs = 5;
    const lowerStateDocExpirationMS = 500;
    for (let conn of [...donorRst.nodes, ...recipientRst.nodes]) {
        setTenantMigrationExpirationParams(
            conn, lowerStateDocExpirationMS, lowerExternalKeysBufferSecs);
    }

    jsTestLog("Donor failover after receiving forgetMigration after updating keys.");
    {
        const [tenantId, migrationId, migrationOpts] = makeTestParams();
        const donorPrimary = tmt.getDonorPrimary();

        assert.commandWorked(tmt.startMigration(migrationOpts));
        assert.commandWorked(
            tmt.waitForMigrationToComplete(migrationOpts, true /* retryOnRetryableErrors */));

        // The keys should have been created without a TTL deadline.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: false});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: false});

        const fp = configureFailPoint(
            donorPrimary, "pauseTenantMigrationDonorBeforeMarkingStateGarbageCollectable");
        const forgetMigrationThread = new Thread(TenantMigrationUtil.forgetMigrationAsync,
                                                 migrationOpts.migrationIdString,
                                                 TenantMigrationUtil.createRstArgs(donorRst),
                                                 true);
        forgetMigrationThread.start();
        fp.wait();

        // Let the keys expire on the donor before the state document is deleted to verify retrying
        // recipientForgetMigration can handle this case. The keys won't be deleted until the buffer
        // expires, so sleep to avoid wasted work.
        sleep((lowerExternalKeysBufferSecs * 1000) + lowerStateDocExpirationMS + 500);
        waitForExternalKeysToBeDeleted(tmt.getDonorPrimary(), migrationId);
        waitForExternalKeysToBeDeleted(tmt.getRecipientPrimary(), migrationId);

        assert.commandWorked(
            donorPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(donorPrimary.adminCommand({replSetFreeze: 0}));
        fp.off();

        assert.commandWorked(forgetMigrationThread.returnData());
    }

    jsTestLog("Recipient failover after receiving forgetMigration after updating keys.");
    {
        const [tenantId, migrationId, migrationOpts] = makeTestParams();
        const recipientPrimary = tmt.getRecipientPrimary();

        assert.commandWorked(tmt.startMigration(migrationOpts));
        assert.commandWorked(
            tmt.waitForMigrationToComplete(migrationOpts, true /* retryOnRetryableErrors */));

        // The keys should have been created without a TTL deadline.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: false});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: false});

        const fp = configureFailPoint(
            recipientPrimary, "fpAfterReceivingRecipientForgetMigration", {action: "hang"});
        const forgetMigrationThread = new Thread(TenantMigrationUtil.forgetMigrationAsync,
                                                 migrationOpts.migrationIdString,
                                                 TenantMigrationUtil.createRstArgs(donorRst),
                                                 true);
        forgetMigrationThread.start();
        fp.wait();

        // Let the keys expire on the donor before the state document is deleted to verify retrying
        // recipientForgetMigration can handle this case. The keys won't be deleted until the buffer
        // expires, so sleep to avoid wasted work.
        sleep((lowerExternalKeysBufferSecs * 1000) + lowerStateDocExpirationMS + 500);
        waitForExternalKeysToBeDeleted(tmt.getRecipientPrimary(), migrationId);

        assert.commandWorked(recipientPrimary.adminCommand(
            {replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(recipientPrimary.adminCommand({replSetFreeze: 0}));
        fp.off();

        assert.commandWorked(forgetMigrationThread.returnData());

        // Eventually the donor's keys should be deleted too.
        waitForExternalKeysToBeDeleted(tmt.getDonorPrimary(), migrationId);
    }

    donorRst.stopSet();
    recipientRst.stopSet();
    tmt.stop();
})();
})();
