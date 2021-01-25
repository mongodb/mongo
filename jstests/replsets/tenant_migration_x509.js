/**
 * Tests that the tenant migration donor authenticates as client to recipient using the
 * migration-specific x.509 certificate, and vice versa.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/libs/uuid_util.js");

function makeTestNs(tenantId) {
    return {dbName: tenantId + "_testDb", collName: "testColl"};
}

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const kDonorCertificateAndPrivateKey =
    TenantMigrationUtil.getCertificateAndPrivateKey("jstests/libs/rs0_tenant_migration.pem");
const kRecipientCertificateAndPrivateKey =
    TenantMigrationUtil.getCertificateAndPrivateKey("jstests/libs/rs1_tenant_migration.pem");

(() => {
    jsTest.log("Test valid donor and recipient certificates");
    const migrationId = UUID();
    const tenantId = "validCertificates";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        donorCertificateForRecipient: kDonorCertificateAndPrivateKey,
        recipientCertificateForDonor: kRecipientCertificateAndPrivateKey,
    };
    const {dbName, collName} = makeTestNs(tenantId);

    tenantMigrationTest.insertDonorDB(dbName, collName);
    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kCommitted);
    tenantMigrationTest.verifyRecipientDB(
        tenantId, dbName, collName, true /* migrationCommitted */);
})();

(() => {
    jsTest.log("Test invalid donor certificate, no header and trailer");
    const migrationId = UUID();
    const tenantId = "invalidDonorCertificateNoHeaderAndTrailer";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        donorCertificateForRecipient: {
            certificate: "invalidCertificate",
            privateKey: kDonorCertificateAndPrivateKey.privateKey
        },
        recipientCertificateForDonor: kRecipientCertificateAndPrivateKey,
    };
    const {dbName, collName} = makeTestNs(tenantId);

    tenantMigrationTest.insertDonorDB(dbName, collName);
    assert.commandFailedWithCode(tenantMigrationTest.runMigration(migrationOpts),
                                 ErrorCodes.InvalidSSLConfiguration);
    tenantMigrationTest.verifyRecipientDB(
        tenantId, dbName, collName, false /* migrationCommitted */);
})();

(() => {
    jsTest.log("Test invalid donor certificate, use private key as certificate");
    const migrationId = UUID();
    const tenantId = "invalidDonorCertificatePrivateKeyAsCertificate";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        donorCertificateForRecipient: {
            certificate: kDonorCertificateAndPrivateKey.privateKey,
            privateKey: kDonorCertificateAndPrivateKey.privateKey
        },
        recipientCertificateForDonor: kRecipientCertificateAndPrivateKey,
    };
    const {dbName, collName} = makeTestNs(tenantId);

    tenantMigrationTest.insertDonorDB(dbName, collName);
    assert.commandFailedWithCode(tenantMigrationTest.runMigration(migrationOpts),
                                 ErrorCodes.InvalidSSLConfiguration);
    tenantMigrationTest.verifyRecipientDB(
        tenantId, dbName, collName, false /* migrationCommitted */);
})();

(() => {
    jsTest.log("Test invalid donor private key, no header and trailer");
    const migrationId = UUID();
    const tenantId = "invalidDonorPrivateKeyNoHeaderAndTrailer";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        donorCertificateForRecipient: {
            certificate: kDonorCertificateAndPrivateKey.certificate,
            privateKey: "invalidCertificate"
        },
        recipientCertificateForDonor: kRecipientCertificateAndPrivateKey,
    };
    const {dbName, collName} = makeTestNs(tenantId);

    tenantMigrationTest.insertDonorDB(dbName, collName);
    assert.commandFailedWithCode(tenantMigrationTest.runMigration(migrationOpts),
                                 ErrorCodes.InvalidSSLConfiguration);
    tenantMigrationTest.verifyRecipientDB(
        tenantId, dbName, collName, false /* migrationCommitted */);
})();

(() => {
    jsTest.log("Test invalid donor private key, use certificate as private key");
    const migrationId = UUID();
    const tenantId = "invalidDonorPrivateKeyCertificateAsPrivateKey";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        donorCertificateForRecipient: {
            certificate: kDonorCertificateAndPrivateKey.certificate,
            privateKey: kDonorCertificateAndPrivateKey.certificate
        },
        recipientCertificateForDonor: kRecipientCertificateAndPrivateKey,
    };
    const {dbName, collName} = makeTestNs(tenantId);

    tenantMigrationTest.insertDonorDB(dbName, collName);
    assert.commandFailedWithCode(tenantMigrationTest.runMigration(migrationOpts),
                                 ErrorCodes.InvalidSSLConfiguration);
    tenantMigrationTest.verifyRecipientDB(
        tenantId, dbName, collName, false /* migrationCommitted */);
})();

(() => {
    jsTest.log("Test invalid donor certificate and private key pair");
    const migrationId = UUID();
    const tenantId = "invalidDonorCertificatePrivateKeyPair";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        donorCertificateForRecipient: {
            certificate: kDonorCertificateAndPrivateKey.certificate,
            privateKey: kRecipientCertificateAndPrivateKey.privateKey
        },
        recipientCertificateForDonor: kRecipientCertificateAndPrivateKey,
    };
    const {dbName, collName} = makeTestNs(tenantId);

    tenantMigrationTest.insertDonorDB(dbName, collName);
    assert.commandFailedWithCode(tenantMigrationTest.runMigration(migrationOpts),
                                 ErrorCodes.InvalidSSLConfiguration);
    tenantMigrationTest.verifyRecipientDB(
        tenantId, dbName, collName, false /* migrationCommitted */);
})();

(() => {
    jsTest.log("Test expired donor certificate and key");
    const migrationId = UUID();
    const tenantId = "expiredDonorCertificate";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        donorCertificateForRecipient: TenantMigrationUtil.getCertificateAndPrivateKey(
            "jstests/libs/rs0_tenant_migration_expired.pem"),
        recipientCertificateForDonor: kRecipientCertificateAndPrivateKey,
    };
    const {dbName, collName} = makeTestNs(tenantId);

    tenantMigrationTest.insertDonorDB(dbName, collName);
    assert.commandFailedWithCode(tenantMigrationTest.runMigration(migrationOpts),
                                 ErrorCodes.InvalidSSLConfiguration);
    tenantMigrationTest.verifyRecipientDB(
        tenantId, dbName, collName, false /* migrationCommitted */);
})();

(() => {
    jsTest.log("Test invalid recipient certificate, no header and trailer");
    const migrationId = UUID();
    const tenantId = "invalidRecipientCertificateNoHeaderAndTrailer";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        donorCertificateForRecipient: kDonorCertificateAndPrivateKey,
        recipientCertificateForDonor: {
            certificate: "invalidCertificate",
            privateKey: kRecipientCertificateAndPrivateKey.privateKey
        },
    };
    const {dbName, collName} = makeTestNs(tenantId);

    tenantMigrationTest.insertDonorDB(dbName, collName);
    assert.commandFailedWithCode(tenantMigrationTest.runMigration(migrationOpts),
                                 ErrorCodes.InvalidSSLConfiguration);
    tenantMigrationTest.verifyRecipientDB(
        tenantId, dbName, collName, false /* migrationCommitted */);
})();

(() => {
    jsTest.log("Test invalid recipient certificate, use private key as certificate");
    const migrationId = UUID();
    const tenantId = "invalidRecipientCertificatePrivateKeyAsCertificate";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        donorCertificateForRecipient: kDonorCertificateAndPrivateKey,
        recipientCertificateForDonor: {
            certificate: kRecipientCertificateAndPrivateKey.privateKey,
            privateKey: kRecipientCertificateAndPrivateKey.privateKey
        },
    };
    const {dbName, collName} = makeTestNs(tenantId);

    tenantMigrationTest.insertDonorDB(dbName, collName);
    assert.commandFailedWithCode(tenantMigrationTest.runMigration(migrationOpts),
                                 ErrorCodes.InvalidSSLConfiguration);
    tenantMigrationTest.verifyRecipientDB(
        tenantId, dbName, collName, false /* migrationCommitted */);
})();

(() => {
    jsTest.log("Test invalid recipient private key, no header and trailer");
    const migrationId = UUID();
    const tenantId = "invalidRecipientPrivateKeyNoHeaderAndTrailer";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        donorCertificateForRecipient: kDonorCertificateAndPrivateKey,
        recipientCertificateForDonor: {
            certificate: kRecipientCertificateAndPrivateKey.certificate,
            privateKey: "invalidCertificate"
        },
    };
    const {dbName, collName} = makeTestNs(tenantId);

    tenantMigrationTest.insertDonorDB(dbName, collName);
    assert.commandFailedWithCode(tenantMigrationTest.runMigration(migrationOpts),
                                 ErrorCodes.InvalidSSLConfiguration);
    tenantMigrationTest.verifyRecipientDB(
        tenantId, dbName, collName, false /* migrationCommitted */);
})();

(() => {
    jsTest.log("Test invalid recipient private key, use certificate as private key");
    const migrationId = UUID();
    const tenantId = "invalidRecipientPrivateKeyCertificateAsPrivateKey";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        donorCertificateForRecipient: kDonorCertificateAndPrivateKey,
        recipientCertificateForDonor: {
            certificate: kRecipientCertificateAndPrivateKey.certificate,
            privateKey: kRecipientCertificateAndPrivateKey.certificate
        },
    };
    const {dbName, collName} = makeTestNs(tenantId);

    tenantMigrationTest.insertDonorDB(dbName, collName);
    assert.commandFailedWithCode(tenantMigrationTest.runMigration(migrationOpts),
                                 ErrorCodes.InvalidSSLConfiguration);
    tenantMigrationTest.verifyRecipientDB(
        tenantId, dbName, collName, false /* migrationCommitted */);
})();

(() => {
    jsTest.log("Test expired recipient certificate and key");
    const migrationId = UUID();
    const tenantId = "expiredRecipientCertificate";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        donorCertificateForRecipient: kDonorCertificateAndPrivateKey,
        recipientCertificateForDonor: TenantMigrationUtil.getCertificateAndPrivateKey(
            "jstests/libs/rs1_tenant_migration_expired.pem"),
    };
    const {dbName, collName} = makeTestNs(tenantId);

    tenantMigrationTest.insertDonorDB(dbName, collName);
    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);
    assert.eq(stateRes.abortReason.code, ErrorCodes.InvalidSSLConfiguration);
    tenantMigrationTest.verifyRecipientDB(
        tenantId, dbName, collName, false /* migrationCommitted */);
})();

(() => {
    jsTest.log("Test invalid recipient certificate and private key pair");
    const migrationId = UUID();
    const tenantId = "invalidRecipientCertificatePrivateKeyPair";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        donorCertificateForRecipient: kDonorCertificateAndPrivateKey,
        recipientCertificateForDonor: {
            certificate: kRecipientCertificateAndPrivateKey.certificate,
            privateKey: kDonorCertificateAndPrivateKey.privateKey
        }
    };
    const {dbName, collName} = makeTestNs(tenantId);

    tenantMigrationTest.insertDonorDB(dbName, collName);
    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);
    assert.eq(stateRes.abortReason.code, ErrorCodes.InvalidSSLConfiguration);
    tenantMigrationTest.verifyRecipientDB(
        tenantId, dbName, collName, false /* migrationCommitted */);
})();

if (!TestData.auth) {
    jsTestLog("Skipping testing authorization since auth is not enabled");
    tenantMigrationTest.stop();
    return;
}

(() => {
    jsTest.log("Test donor certificate without findInternalClusterTimeKeysRole role");
    const migrationId = UUID();
    const tenantId = "donorCertificateNoFindInternalClusterTimeKeysRole";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        donorCertificateForRecipient: TenantMigrationUtil.getCertificateAndPrivateKey(
            "jstests/libs/rs0_tenant_migration_no_find_cluster_time_keys_role.pem"),
        recipientCertificateForDonor: kRecipientCertificateAndPrivateKey,
    };
    const {dbName, collName} = makeTestNs(tenantId);

    tenantMigrationTest.insertDonorDB(dbName, collName);
    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);
    assert.eq(stateRes.abortReason.code, ErrorCodes.Unauthorized);
    tenantMigrationTest.verifyRecipientDB(
        tenantId, dbName, collName, false /* migrationCommitted */);
})();

(() => {
    jsTest.log("Test recipient certificate without backup role");
    const migrationId = UUID();
    const tenantId = "recipientCertificateNoBackupRole";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        donorCertificateForRecipient: kDonorCertificateAndPrivateKey,
        recipientCertificateForDonor: TenantMigrationUtil.getCertificateAndPrivateKey(
            "jstests/libs/rs1_tenant_migration_no_backup_role.pem"),
    };
    const {dbName, collName} = makeTestNs(tenantId);

    tenantMigrationTest.insertDonorDB(dbName, collName);
    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);
    assert.eq(stateRes.abortReason.code, ErrorCodes.Unauthorized);
    tenantMigrationTest.verifyRecipientDB(
        tenantId, dbName, collName, false /* migrationCommitted */);
})();

(() => {
    jsTest.log("Test recipient certificate without findInternalClusterTimeKeysRole role");
    const migrationId = UUID();
    const tenantId = "recipientCertificateNoFindInternalClusterTimeKeysRole";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        donorCertificateForRecipient: kDonorCertificateAndPrivateKey,
        recipientCertificateForDonor: TenantMigrationUtil.getCertificateAndPrivateKey(
            "jstests/libs/rs1_tenant_migration_no_find_cluster_time_keys_role.pem"),
    };
    const {dbName, collName} = makeTestNs(tenantId);

    tenantMigrationTest.insertDonorDB(dbName, collName);
    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);
    assert.eq(stateRes.abortReason.code, ErrorCodes.Unauthorized);
    tenantMigrationTest.verifyRecipientDB(
        tenantId, dbName, collName, false /* migrationCommitted */);
})();

tenantMigrationTest.stop();
})();
