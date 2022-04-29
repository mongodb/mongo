/**
 * Tests that large write error results from bulk write operations are within the BSON size limit.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest.
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
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kCollName = "testColl";
const kTenantDefinedDbName = "0";

function bulkWriteDocsUnordered(primaryHost, dbName, collName, numDocs) {
    const primary = new Mongo(primaryHost);
    let primaryDB = primary.getDB(dbName);

    let batch = [];
    for (let i = 0; i < numDocs; ++i) {
        batch.push({x: i});
    }

    let request = {insert: collName, documents: batch, writeConcern: {w: 1}, ordered: false};
    res = assert.commandFailedWithCode(primaryDB[collName].runCommand(request),
                                       ErrorCodes.TenantMigrationCommitted);

    return res;
}

jsTestLog("Testing that large write errors fit within the BSON size limit.");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const tenantId = "bulkUnorderedInserts-committed";
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(UUID()),
    tenantId,
};

const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
const primary = tenantMigrationTest.getDonorPrimary();
const primaryDB = primary.getDB(dbName);
const numWriteOps =
    assert.commandWorked(primaryDB.hello()).maxWriteBatchSize;  // num of writes to run in bulk.

assert.commandWorked(primaryDB.runCommand({create: kCollName}));

// Do a large unordered bulk insert that fails all inserts in order to generate a large write
// result.
const writeFp = configureFailPoint(primaryDB, "hangDuringBatchInsert");
const bulkWriteThread =
    new Thread(bulkWriteDocsUnordered, primary.host, dbName, kCollName, numWriteOps);

bulkWriteThread.start();
writeFp.wait();

TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

writeFp.off();
bulkWriteThread.join();

const bulkWriteRes = bulkWriteThread.returnData();
const writeErrors = bulkWriteRes.writeErrors;

assert.gt(writeErrors.length, 0);

writeErrors.forEach((err, arrIndex) => {
    assert.eq(err.code, ErrorCodes.TenantMigrationCommitted);
    if (arrIndex == 0) {
        assert(err.errmsg);
    } else {
        assert(!err.errmsg);
    }
});

// This assert is more or less a sanity check since jsThreads need to convert data it returns
// into a BSON object. So if we have reached this assert, we already know that the write result
// is within the BSON limits.
assert.lte(Object.bsonsize(bulkWriteRes),
           assert.commandWorked(primaryDB.hello()).maxBsonObjectSize);

tenantMigrationTest.stop();
})();
