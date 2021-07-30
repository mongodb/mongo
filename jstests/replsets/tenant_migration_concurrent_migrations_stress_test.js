/**
 * Stress test runs many concurrent migrations against the same recipient.
 * @tags: [
 *   incompatible_with_amazon_linux,
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");  // for 'extractUUIDFromObject'
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/replsets/rslib.js");  // for 'setLogVerbosity'

const kMigrationsCount = 300;
const kConcurrentMigrationsCount = 120;

// An object that mirrors the donor migration states.
const migrationStates = {
    kUninitialized: 0,
    kAbortingIndexBuilds: 1,
    kDataSync: 2,
    kBlocking: 3,
    kCommitted: 4,
    kAborted: 5
};

const setParameterOpts = {
    maxTenantMigrationRecipientThreadPoolSize: 1000,
    maxTenantMigrationDonorServiceThreadPoolSize: 1000
};
const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), sharedOptions: {setParameter: setParameterOpts}});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

setLogVerbosity([donorPrimary, recipientPrimary], {
    "tenantMigration": {"verbosity": 0},
    "replication": {"verbosity": 0},
    "sharding": {"verbosity": 0}
});

// Set up tenant data for the migrations.
const tenantIds = [...Array(kMigrationsCount).keys()].map((i) => `testTenantId-${i}`);
let migrationOptsArray = [];
tenantIds.forEach((tenantId) => {
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const collName = "testColl";
    tenantMigrationTest.insertDonorDB(dbName, collName, [{_id: 1}]);
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId: tenantId,
    };
    migrationOptsArray.push(migrationOpts);
});

// Start the migrations.
let nextMigration = 0;
let runningMigrations = 0;
let setOfCompleteMigrations = new Set();
let didFirstLoopSleep = false;
const regexId = /testTenantId-([0-9]+)/;

while (setOfCompleteMigrations.size < kMigrationsCount) {
    while (runningMigrations < kConcurrentMigrationsCount && nextMigration < kMigrationsCount) {
        jsTestLog("Starting migration for tenant: " + migrationOptsArray[nextMigration].tenantId);
        assert.commandWorked(tenantMigrationTest.startMigration(migrationOptsArray[nextMigration]));
        ++nextMigration;
        ++runningMigrations;
    }
    if (didFirstLoopSleep === false ||
        runningMigrations + setOfCompleteMigrations.size == kMigrationsCount) {
        didFirstLoopSleep = true;
        // After starting many migrations first time there is no need to check soon.
        // The second case is when all migrations are already scheduled.
        sleep(5 * 1000);
    }
    sleep(100);  // Do not query too often in any case.

    assert.soon(function() {
        let currentOp = tenantMigrationTest.getDonorPrimary().adminCommand(
            {currentOp: true, desc: "tenant donor migration"});
        if (!currentOp.ok) {
            return false;
        }
        currentOp.inprog.forEach((op) => {
            if (op.lastDurableState === migrationStates.kCommitted) {
                let found = op.tenantId.match(regexId);
                assert(found !== null);
                assert.eq(found.length, 2);
                let id = parseInt(found[1]);
                assert(id >= 0, `${id}`);
                assert(id <= kMigrationsCount, `${id}`);
                // Check if this migration completed after previous check.
                if (!setOfCompleteMigrations.has(id)) {
                    setOfCompleteMigrations.add(id);
                    --runningMigrations;
                }
            }
        });
        return true;
    });

    jsTestLog("Currently running " + runningMigrations + ", complete count " +
              setOfCompleteMigrations.size);
}

// Wait and forget all migrations.
for (let i = 0; i < kMigrationsCount; ++i) {
    // All migrations should be done, wait.
    jsTestLog("Waiting for migration for tenant: " + migrationOptsArray[i].tenantId +
              " to complete.");
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOptsArray[i]));

    // Forget migrations first before shutting down the test to prevent unnecessary failover
    // retries.
    jsTestLog("Forgetting migration for tenant: " + migrationOptsArray[i].tenantId);
    assert.commandWorked(
        tenantMigrationTest.forgetMigration(migrationOptsArray[i].migrationIdString));
}

tenantMigrationTest.stop();
})();
