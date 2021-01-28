/**
 * Test that tenant migration donor and recipient correctly copy each other cluster time keys into
 * their admin.system.external_validation_keys collection.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const kInternalKeysNs = "admin.system.keys";
const kExternalKeysNs = "admin.system.external_validation_keys";

/**
 * Asserts that the donor and recipient have copied each other's cluster time keys into
 * admin.system.external_validation_keys.
 */
function assertCopiedExternalKeys(tenantMigrationTest) {
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    recipientPrimary.getCollection(kInternalKeysNs).find().forEach(internalKeyDoc => {
        assert.neq(null, donorPrimary.getCollection(kExternalKeysNs).findOne({
            keyId: internalKeyDoc._id,
            key: internalKeyDoc.key,
            expiresAt: internalKeyDoc.expiresAt,
            replicaSetName: tenantMigrationTest.getRecipientRst().name
        }));
    });

    donorPrimary.getCollection(kInternalKeysNs).find().forEach(internalKeyDoc => {
        assert.neq(null, recipientPrimary.getCollection(kExternalKeysNs).findOne({
            keyId: internalKeyDoc._id,
            key: internalKeyDoc.key,
            expiresAt: internalKeyDoc.expiresAt,
            replicaSetName: tenantMigrationTest.getDonorRst().name
        }));
    });
}

const kTenantId = "testTenantId";
const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();

(() => {
    jsTest.log("Test that the donor and recipient correctly copy each other's cluster time keys " +
               "when there is no failover.");
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        tenantMigrationTest.stop();
        return;
    }

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
    };
    assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assertCopiedExternalKeys(tenantMigrationTest);

    tenantMigrationTest.stop();
})();

(() => {
    jsTest.log("Test that the donor and recipient correctly copy each other's cluster time keys " +
               "when there is no failover but the recipient syncs data from a secondary.");
    const recipientRst = new ReplSetTest(
        {nodes: 3, name: "recipientRst", nodeOptions: migrationX509Options.recipient});
    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), recipientRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        recipientRst.stopSet();
        tenantMigrationTest.stop();
        return;
    }

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        readPreference: {mode: "secondary"}
    };
    assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assertCopiedExternalKeys(tenantMigrationTest);

    recipientRst.stopSet();
    tenantMigrationTest.stop();
})();

(() => {
    jsTest.log("Test that the donor and recipient correctly copy each other's cluster time keys " +
               "when there is donor failover.");
    const donorRst =
        new ReplSetTest({nodes: 3, name: "donorRst", nodeOptions: migrationX509Options.donor});
    donorRst.startSet();
    donorRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        tenantMigrationTest.stop();
        return;
    }

    let donorPrimary = donorRst.getPrimary();
    const fp =
        configureFailPoint(donorPrimary, "pauseTenantMigrationAfterPersitingInitialDonorStateDoc");

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
    };
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    fp.wait();

    assert.commandWorked(
        donorPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    assert.commandWorked(donorPrimary.adminCommand({replSetFreeze: 0}));

    fp.off();
    assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(
        migrationOpts, true /* retryOnRetryableErrors */));

    assertCopiedExternalKeys(tenantMigrationTest);

    donorRst.stopSet();
    tenantMigrationTest.stop();
})();

(() => {
    jsTest.log("Test that the donor and recipient correctly copy each other's cluster time keys " +
               "when there is recipient failover.");
    const recipientRst = new ReplSetTest(
        {nodes: 3, name: "recipientRst", nodeOptions: migrationX509Options.recipient});
    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), recipientRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        recipientRst.stopSet();
        tenantMigrationTest.stop();
        return;
    }

    const recipientPrimary = recipientRst.getPrimary();
    const fp = configureFailPoint(recipientPrimary,
                                  "fpAfterPersistingTenantMigrationRecipientInstanceStateDoc",
                                  {action: "hang"});

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
    };
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    fp.wait();

    assert.commandWorked(
        recipientPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    assert.commandWorked(recipientPrimary.adminCommand({replSetFreeze: 0}));

    fp.off();
    assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(
        migrationOpts, true /* retryOnRetryableErrors */));

    assertCopiedExternalKeys(tenantMigrationTest);

    recipientRst.stopSet();
    tenantMigrationTest.stop();
})();
})();
