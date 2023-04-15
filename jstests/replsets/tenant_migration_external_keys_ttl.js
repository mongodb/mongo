/**
 * Tests that tenant migrations correctly set the TTL values for keys in the
 * config.external_validation_keys collection.
 *
 * TODO SERVER-61231: shard merge can't handle concurrent migrations, adapt this test.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    forgetMigrationAsync,
    getExternalKeys,
    isShardMergeEnabled,
    kExternalKeysNs,
    makeX509OptionsForTest
} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/replsets/rslib.js");  // `createRstArgs`

const kExternalKeysTTLIndexName = "ExternalKeysTTLIndex";
const ttlMonitorOptions = {
    ttlMonitorSleepSecs: 1
};

let makeTenantId = function() {
    return ObjectId().str;
};

function waitForExternalKeysTTLIndex(conn) {
    assert.soon(() => {
        const indexSpecs = conn.getCollection(kExternalKeysNs).getIndexSpecs();
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
        const externalKeys = getExternalKeys(conn, migrationId);
        assert.eq(0, externalKeys.length, tojson(externalKeys));
        return true;
    });
}

function verifyExternalKeys(conn, {migrationId, expectTTLValue}) {
    const externalKeys = conn.getCollection(kExternalKeysNs).find({migrationId}).toArray();
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
    function setup() {
        const tmt = new TenantMigrationTest(
            {name: jsTestName(), sharedOptions: {setParameter: ttlMonitorOptions}});

        // Verify the external keys TTL index is created on both replica sets on stepup.
        waitForExternalKeysTTLIndex(tmt.getDonorPrimary());
        waitForExternalKeysTTLIndex(tmt.getRecipientPrimary());

        return {
            tmt,
            teardown: function() {
                tmt.stop();
            },
        };
    }

    (() => {
        jsTestLog("Basic case with multiple migrations");
        const {tmt, teardown} = setup();
        if (isShardMergeEnabled(tmt.getDonorPrimary().getDB("admin"))) {
            // This test runs multiple concurrent migrations, which shard merge can't handle.
            jsTestLog(
                "Skip: featureFlagShardMerge is enabled and this test runs multiple concurrent migrations, which shard merge can't handle.");
            teardown();
            return;
        }
        const [tenantId, migrationId, migrationOpts] = makeTestParams();

        TenantMigrationTest.assertCommitted(
            tmt.runMigration(migrationOpts, {automaticForgetMigration: false}));

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

        TenantMigrationTest.assertCommitted(
            tmt.runMigration(otherMigrationOpts, {automaticForgetMigration: false}));

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
        teardown();
    })();

    jsTestLog("Verify the TTL value is respected and keys are eventually reaped");
    {
        const {tmt, teardown} = setup();
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

        TenantMigrationTest.assertCommitted(
            tmt.runMigration(migrationOpts, {automaticForgetMigration: false}));

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
        teardown();
    }
})();

//
// Tests with failovers
//

(() => {
    function setup() {
        const migrationX509Options = makeX509OptionsForTest();
        const donorRst = new ReplSetTest({
            nodes: 3,
            name: "donorRst",
            serverless: true,
            nodeOptions:
                Object.assign(migrationX509Options.donor, {setParameter: ttlMonitorOptions})
        });
        donorRst.startSet();
        donorRst.initiate();

        const recipientRst = new ReplSetTest({
            nodes: 3,
            name: "recipientRst",
            serverless: true,
            nodeOptions:
                Object.assign(migrationX509Options.recipient, {setParameter: ttlMonitorOptions})
        });
        recipientRst.startSet();
        recipientRst.initiate();

        const tmt = new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});

        return {
            tmt,
            donorRst,
            recipientRst,
            teardown: function() {
                donorRst.stopSet();
                recipientRst.stopSet();
                tmt.stop();
            },
        };
    }

    jsTestLog("Donor failover before receiving forgetMigration");
    {
        const {tmt, teardown} = setup();
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

        TenantMigrationTest.assertCommitted(
            tmt.waitForMigrationToComplete(migrationOpts, true /* retryOnRetryableErrors */));

        // The keys should have been created without a TTL deadline.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: false});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: false});

        assert.commandWorked(tmt.forgetMigration(migrationOpts.migrationIdString));

        // After running donorForgetMigration, the TTL value should be updated. The default TTL
        // buffer is 1 day so the keys will not have been deleted.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: true});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: true});
        teardown();
    }

    jsTestLog("Recipient failover before receiving forgetMigration");
    {
        const {tmt, teardown} = setup();
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

        TenantMigrationTest.assertCommitted(
            tmt.waitForMigrationToComplete(migrationOpts, true /* retryOnRetryableErrors */));

        // The keys should have been created without a TTL deadline.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: false});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: false});

        assert.commandWorked(tmt.forgetMigration(migrationOpts.migrationIdString));

        // After running donorForgetMigration, the TTL value should be updated. The default TTL
        // buffer is 1 day so the keys will not have been deleted.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: true});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: true});
        teardown();
    }

    jsTestLog(
        "Donor failover after receiving forgetMigration before marking keys garbage collectable");
    {
        const {tmt, donorRst, teardown} = setup();
        const [tenantId, migrationId, migrationOpts] = makeTestParams();
        const donorPrimary = tmt.getDonorPrimary();

        assert.commandWorked(tmt.startMigration(migrationOpts));
        TenantMigrationTest.assertCommitted(
            tmt.waitForMigrationToComplete(migrationOpts, true /* retryOnRetryableErrors */));

        // The keys should have been created without a TTL deadline.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: false});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: false});

        const fp = configureFailPoint(
            donorPrimary, "pauseTenantMigrationBeforeMarkingExternalKeysGarbageCollectable");
        const forgetMigrationThread = new Thread(
            forgetMigrationAsync, migrationOpts.migrationIdString, createRstArgs(donorRst), true);
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
        teardown();
    }

    jsTestLog(
        "Recipient failover after receiving forgetMigration before marking keys garbage collectable");
    {
        const {tmt, donorRst, teardown} = setup();
        const [tenantId, migrationId, migrationOpts] = makeTestParams();
        const recipientPrimary = tmt.getRecipientPrimary();

        assert.commandWorked(tmt.startMigration(migrationOpts));
        TenantMigrationTest.assertCommitted(
            tmt.waitForMigrationToComplete(migrationOpts, true /* retryOnRetryableErrors */));

        // The keys should have been created without a TTL deadline.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: false});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: false});

        const fp = configureFailPoint(
            recipientPrimary, "pauseTenantMigrationBeforeMarkingExternalKeysGarbageCollectable");
        const forgetMigrationThread = new Thread(
            forgetMigrationAsync, migrationOpts.migrationIdString, createRstArgs(donorRst), true);
        forgetMigrationThread.start();
        fp.wait();

        assert.commandWorked(recipientPrimary.adminCommand(
            {replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(recipientPrimary.adminCommand({replSetFreeze: 0}));
        fp.off();

        assert.commandWorked(forgetMigrationThread.returnData());

        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: true});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: true});
        teardown();
    }

    jsTestLog("Donor failover after receiving forgetMigration after updating keys.");
    {
        const {tmt, donorRst, recipientRst, teardown} = setup();
        // this test expects the external keys to expire, so lower the expiration timeouts.
        const lowerExternalKeysBufferSecs = 5;
        const lowerStateDocExpirationMS = 500;
        for (let conn of [...donorRst.nodes, ...recipientRst.nodes]) {
            setTenantMigrationExpirationParams(
                conn, lowerStateDocExpirationMS, lowerExternalKeysBufferSecs);
        }
        const [tenantId, migrationId, migrationOpts] = makeTestParams();
        const donorPrimary = tmt.getDonorPrimary();

        assert.commandWorked(tmt.startMigration(migrationOpts));
        TenantMigrationTest.assertCommitted(
            tmt.waitForMigrationToComplete(migrationOpts, true /* retryOnRetryableErrors */));

        // The keys should have been created without a TTL deadline.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: false});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: false});

        const fp = configureFailPoint(
            donorPrimary, "pauseTenantMigrationDonorBeforeMarkingStateGarbageCollectable");
        const forgetMigrationThread = new Thread(
            forgetMigrationAsync, migrationOpts.migrationIdString, createRstArgs(donorRst), true);
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
        teardown();
    }

    jsTestLog("Recipient failover after receiving forgetMigration after updating keys.");
    {
        const {tmt, donorRst, recipientRst, teardown} = setup();
        // this test expects the external keys to expire, so lower the expiration timeouts.
        const lowerExternalKeysBufferSecs = 5;
        const lowerStateDocExpirationMS = 500;
        for (let conn of [...donorRst.nodes, ...recipientRst.nodes]) {
            setTenantMigrationExpirationParams(
                conn, lowerStateDocExpirationMS, lowerExternalKeysBufferSecs);
        }
        const [tenantId, migrationId, migrationOpts] = makeTestParams();
        const recipientPrimary = tmt.getRecipientPrimary();

        assert.commandWorked(tmt.startMigration(migrationOpts));
        TenantMigrationTest.assertCommitted(
            tmt.waitForMigrationToComplete(migrationOpts, true /* retryOnRetryableErrors */));

        // The keys should have been created without a TTL deadline.
        verifyExternalKeys(tmt.getDonorPrimary(), {migrationId, expectTTLValue: false});
        verifyExternalKeys(tmt.getRecipientPrimary(), {migrationId, expectTTLValue: false});

        const fp = configureFailPoint(
            recipientPrimary, "fpAfterReceivingRecipientForgetMigration", {action: "hang"});
        const forgetMigrationThread = new Thread(
            forgetMigrationAsync, migrationOpts.migrationIdString, createRstArgs(donorRst), true);
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
        teardown();
    }
})();
