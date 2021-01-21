/**
 * Test that tenant migration donor correctly copies recipient's cluster time keys into its
 * admin.system.external_validation_keys collection.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const kInternalKeysNs = "admin.system.keys";
const kExternalKeysNs = "admin.system.external_validation_keys";

/**
 * Asserts that the donor has copied all the recipient's keys into
 * admin.system.external_validation_keys.
 */
function assertDonorCopiedExternalKeys(tenantMigrationTest) {
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
}

const kTenantId = "testTenantId";
const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();

(() => {
    jsTest.log("Test that the donor correctly copies the recipient's cluster time keys " +
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
    assertDonorCopiedExternalKeys(tenantMigrationTest);

    tenantMigrationTest.stop();
})();

(() => {
    jsTest.log("Test that the donor correctly copies the recipient's cluster time keys " +
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

    assertDonorCopiedExternalKeys(tenantMigrationTest);

    donorRst.stopSet();
    tenantMigrationTest.stop();
})();
})();
