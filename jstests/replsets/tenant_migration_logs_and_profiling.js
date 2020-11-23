/**
 * Tests that migration certificates do not show up in the logs or system.profile collections.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/libs/uuid_util.js");

function assertNoCertificateOrPrivateKeyLogsForCmd(conn, cmdName) {
    assert(checkLog.checkContainsOnce(conn, new RegExp(`Slow query.*${cmdName}`)),
           "did not find slow query logs for the command");
    assert(!checkLog.checkContainsOnce(conn, /BEGIN CERTIFICATE.*END CERTIFICATE/),
           "found certificate in the logs");
    assert(!checkLog.checkContainsOnce(conn, /BEGIN PRIVATE KEY.*END PRIVATE KEY/),
           "found private key in the logs");
}

function assertNoCertificateOrPrivateKeyFields(doc) {
    for (let k of Object.keys(doc)) {
        let v = doc[k];
        if (typeof v === "string") {
            assert(!v.match(/BEGIN CERTIFICATE(.*\n.*)*END CERTIFICATE/m),
                   `found certificate field`);
            assert(!v.match(/BEGIN PRIVATE KEY(.*\n.*)*END PRIVATE KEY/m),
                   `found private key field`);
        } else if (Array.isArray(v)) {
            v.forEach((item) => {
                if (typeof item === "object" && item !== null) {
                    assertNoCertificateOrPrivateKeyFields(v);
                }
            });
        } else if (typeof v === "object" && v !== null) {
            assertNoCertificateOrPrivateKeyFields(v);
        }
    }
}

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const donorPrimary = tenantMigrationTest.getDonorPrimary();

// Verify that migration certificates are not logged as part of slow query logging.
(() => {
    const donorDefaultSlowMs =
        assert.commandWorked(donorPrimary.adminCommand({profile: 0, slowms: 0})).slowms;

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: "slowCommands",
    };

    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kCommitted);

    assertNoCertificateOrPrivateKeyLogsForCmd(donorPrimary, "donorStartMigration");

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    assertNoCertificateOrPrivateKeyLogsForCmd(donorPrimary, "donorForgetMigration");

    assert.commandWorked(donorPrimary.adminCommand({profile: 0, slowms: donorDefaultSlowMs}));
})();

// Verify that migration certificates do not show up in system.profile collections.
(() => {
    const donorAdminDB = donorPrimary.getDB("config");

    donorAdminDB.setProfilingLevel(2);

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: "profiler",
    };

    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kCommitted);

    donorAdminDB.system.profile.find({ns: TenantMigrationTest.kConfigDonorsNS}).forEach(doc => {
        assertNoCertificateOrPrivateKeyFields(doc);
    });

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    donorAdminDB.system.profile.find({ns: TenantMigrationTest.kConfigDonorsNS}).forEach(doc => {
        assertNoCertificateOrPrivateKeyFields(doc);
    });
})();

tenantMigrationTest.stop();
})();
