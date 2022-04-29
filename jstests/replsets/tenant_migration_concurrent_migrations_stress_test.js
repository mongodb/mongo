/**
 * Stress test runs many concurrent migrations against the same recipient.
 * @tags: [
 *   incompatible_with_amazon_linux,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
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

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

if (TenantMigrationUtil.isShardMergeEnabled(donorPrimary.getDB("admin"))) {
    // This test runs multiple concurrent migrations, which shard merge can't handle.
    jsTestLog(
        "Skip: featureFlagShardMerge is enabled and this test runs multiple concurrent migrations, which shard merge can't handle.");
    tenantMigrationTest.stop();
    return;
}

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

// Blocks until the migration with index `id` completes (it is supposed to be aborted so
// the wait should be short) and creates another migration.
function retryAbortedMigration(id) {
    let tenantId = migrationOptsArray[id].tenantId;
    jsTestLog(
        `Forgetting and restarting aborted migration
        ${migrationOptsArray[id].migrationIdString} for tenant: ${tenantId}`);
    let waitState = tenantMigrationTest.waitForMigrationToComplete(migrationOptsArray[id]);
    assert.commandWorked(waitState);
    if (waitState.state != TenantMigrationTest.DonorState.kAborted) {
        // The `currentOp()` seems to be lagging so this condition actually happens.
        // We simply ignore this condition.
        // Note: this is not a bug, the code is fast enough to forget, replace Id
        // and restart the migration with the same name to get a stale currentOp() result
        // from previous attempt with same name. As we replace UUID() below it is guaranteed
        // that we do not restart the same migration and 'aborted' state is terminal. The
        // currentOp() is stale because forgetting the migration only marks it for garbage
        // collection, which happens later.
        jsTestLog(`Migration was supposed to be aborted, got: ${tojson(waitState)}`);
        return;
    }

    assert.commandWorked(
        tenantMigrationTest.forgetMigration(migrationOptsArray[id].migrationIdString));

    // Drop recipient DB.
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    let db = recipientPrimary.getDB(dbName);
    try {
        db.dropDatabase();
    } catch (err) {
        jsTestLog(`Dropping recipient DB: ${tojson(err)}`);
    }

    // Replace migration UUID.
    migrationOptsArray[id].migrationIdString = extractUUIDFromObject(UUID());
    // Old migration needs to be garbage collected before this works.
    assert.soon(function() {
        let status = tenantMigrationTest.startMigration(migrationOptsArray[id]);
        if (!status.ok) {
            jsTestLog(`${tojson(status)}`);
        }
        return status.ok;
    }, 'Failed to start', 60 * 1000, 5 * 1000);
}

// Start the migrations.
let nextMigration = 0;
let runningMigrations = 0;
let setOfCompleteMigrations = new Set();
let didFirstLoopSleep = false;
const regexId = /testTenantId-([0-9]+)/;
// Reduce spam by logging the aborted migration once, also use this flag to abort one migration.
let seenAbortedMigration = false;

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

    let migrationsByState = {};  // Map of sets: key is state, value is set of IDs.
    assert.soon(function() {
        let currentOp = tenantMigrationTest.getDonorPrimary().adminCommand(
            {currentOp: true, desc: "tenant donor migration"});
        if (!currentOp.ok) {
            return false;
        }
        currentOp.inprog.forEach((op) => {
            let idPatternFound = op.tenantId.match(regexId);
            assert(idPatternFound !== null);
            assert.eq(idPatternFound.length, 2);
            let id = parseInt(idPatternFound[1]);
            assert(!isNaN(id));
            assert(id >= 0, `${id}`);
            assert(id <= kMigrationsCount, `${id}`);

            if (op.lastDurableState === migrationStates.kCommitted) {
                // Check if this migration completed after previous check.
                if (!setOfCompleteMigrations.has(id)) {
                    setOfCompleteMigrations.add(id);
                    --runningMigrations;
                }
            }

            if (op.lastDurableState === migrationStates.kAborted) {
                if (!seenAbortedMigration) {
                    seenAbortedMigration = true;
                    jsTestLog(`Found an aborted migration in ${tojson(currentOp)}`);
                }
                retryAbortedMigration(id);
            }

            if (!(op.lastDurableState in migrationsByState)) {
                migrationsByState[op.lastDurableState] = new Set();
            }
            let idsForState = migrationsByState[op.lastDurableState];
            idsForState.add(id);
        });
        return true;
    });

    // Abort a random migration until observed by the `currentOp`.
    if (!seenAbortedMigration && migrationStates.kDataSync in migrationsByState &&
        migrationsByState[migrationStates.kDataSync].size > 0) {
        let items = Array.from(migrationsByState[migrationStates.kDataSync]);
        let id = items[Math.floor(Math.random() * items.length)];
        jsTestLog(`${id}`);
        tenantMigrationTest.tryAbortMigration(
            {migrationIdString: migrationOptsArray[id].migrationIdString});
    }

    jsTestLog("Currently running " + runningMigrations + ", complete count " +
              setOfCompleteMigrations.size);

    for (let state in migrationsByState) {
        let ids = migrationsByState[state];
        if (ids.size > 0 && ids.size <= 10) {
            // We only log the small collections to know which ones were stuck.
            jsTestLog(`Migrations in state ${state}: ${JSON.stringify([...ids])}`);
        }
    }
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
