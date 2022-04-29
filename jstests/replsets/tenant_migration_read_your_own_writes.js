/**
 * Tests that non-timestamped reads are not allowed on the donor after the migration has committed
 * so that we typically provide read-your-own-write guarantees for primary reads across tenant
 * migrations when there is no other failover.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/fail_point_util.js");  // For configureFailPoint().

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), allowStaleReadsOnDonor: false});

const kTenantId = "testTenantId";
const kDbName = tenantMigrationTest.tenantDB(kTenantId, "testDB");
const kCollName = "testColl";

const donorPrimary = tenantMigrationTest.getDonorPrimary();

tenantMigrationTest.insertDonorDB(kDbName, kCollName, [...Array(10).keys()].map(x => ({x: x})));

const donorDB = donorPrimary.getDB(kDbName);
const cursor = assert
                   .commandWorked(donorDB.runCommand({
                       find: kCollName,
                       batchSize: 5,
                   }))
                   .cursor;
assert.eq(5, cursor.firstBatch.length, tojson(cursor));
assert.neq(0, cursor.id, tojson(cursor));
jsTestLog(`Started cursor id ${cursor.id} on the donor before the migration`);

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};
TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

// Test that getMore works after the migration has committed.
jsTestLog(`Testing getMore on cursor id ${cursor.id} on the donor after the migration`);
assert.commandWorked(donorDB.runCommand({getMore: cursor.id, collection: kCollName}));

// Test that local and majority reads are no longer allowed on the donor.
const testCases = {
    find: {command: {find: kCollName}},
    count: {command: {count: kCollName}},
    distinct: {command: {distinct: kCollName, key: "x", query: {}}},
    aggregate: {command: {aggregate: kCollName, pipeline: [{$match: {}}], cursor: {}}},
    mapReduce: {
        command: {
            mapReduce: kCollName,
            map: () => {
                emit(this.x, 1);
            },
            reduce: (key, value) => {
                return 1;
            },
            out: {inline: 1}
        },
        skipReadConcernMajority: true,
    },
    findAndModify: {
        command: {findAndModify: kCollName, query: {x: 1}, update: {$set: {x: 1}}},
        skipReadConcernMajority: true,
    },
    update: {
        // No-op write due to stale read is also not allowed.
        command: {update: kCollName, updates: [{q: {x: 1}, u: {$set: {x: 1}}}]},
        skipReadConcernMajority: true,
    },
    delete: {
        // No-op write due to stale read is also not allowed.
        command: {delete: kCollName, deletes: [{q: {x: 100}, limit: 1}]},
        skipReadConcernMajority: true,
    },
    listCollections: {
        command: {listCollections: 1},
        skipReadConcernMajority: true,
    },
    listIndexes: {
        command: {listIndexes: kCollName},
        skipReadConcernMajority: true,
    },
};

const readConcerns = {
    local: {level: "local"},
    majority: {level: "majority"},
};

for (const [testCaseName, testCase] of Object.entries(testCases)) {
    for (const [readConcernName, readConcern] of Object.entries(readConcerns)) {
        if (testCase.skipReadConcernMajority && readConcernName === "majority") {
            continue;
        }
        jsTest.log(`Testing ${testCaseName} with readConcern ${readConcernName}`);
        let cmd = testCase.command;
        cmd.readConcern = readConcern;
        assert.commandFailedWithCode(donorDB.runCommand(cmd), ErrorCodes.TenantMigrationCommitted);
    }
}

// Enable stale reads on the donor set so that end of test data consistency check can pass.
tenantMigrationTest.getDonorRst().nodes.forEach(
    node => assert.commandWorked(node.adminCommand(
        {configureFailPoint: "tenantMigrationDonorAllowsNonTimestampedReads", mode: "alwaysOn"})));

tenantMigrationTest.stop();
})();
