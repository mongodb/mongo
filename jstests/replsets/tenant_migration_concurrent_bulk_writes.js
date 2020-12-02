/**
 * Tests that bulk writes during a tenant migration correctly report write errors and
 * retries writes that returned TenantMigrationCommitted.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kMaxBatchSize = 2;
const kCollName = "testColl";
const kTenantDefinedDbName = "0";
const kNumWriteOps = 6;  // num of writes to run in bulk.
const kNumWriteBatchesWithoutMigrationConflict =
    2;  // num of write batches we allow to complete before migration blocks writes.
const kMaxSleepTimeMS = 1000;
const kBatchTypes = {
    insert: 1,
    update: 2,
    remove: 3
};

const donorRst = new ReplSetTest({
    nodes: 1,
    name: 'donor',
    nodeOptions: {
        setParameter: {
            internalInsertMaxBatchSize:
                kMaxBatchSize /* Decrease internal max batch size so we can still show writes are
                                 batched without inserting hundreds of documents. */
        }
    }
});
const recipientRst = new ReplSetTest({
    nodes: 1,
    name: 'recipient',
    nodeOptions: {
        setParameter: {
            internalInsertMaxBatchSize:
                kMaxBatchSize /* Decrease internal max batch size so we can still show writes are
                                 batched without inserting hundreds of documents. */
        },
    }
});
const kRecipientConnString = recipientRst.getURL();

function bulkWriteDocsOrdered(primaryHost, dbName, collName, numDocs) {
    const primary = new Mongo(primaryHost);
    let primaryDB = primary.getDB(dbName);
    let bulk = primaryDB[collName].initializeOrderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        bulk.insert({x: i});
    }

    let res;
    try {
        res = bulk.execute();
    } catch (e) {
        res = e;
    }
    return {res: res.getRawResponse(), ops: bulk.getOperations()};
}

function bulkWriteDocsUnordered(primaryHost, dbName, collName, numDocs) {
    const primary = new Mongo(primaryHost);
    let primaryDB = primary.getDB(dbName);
    let bulk = primaryDB[collName].initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        bulk.insert({x: i});
    }

    let res;
    try {
        res = bulk.execute();
    } catch (e) {
        res = e;
    }
    return {res: res.getRawResponse(), ops: bulk.getOperations()};
}

/**
 * TODO SERVER-51764: Refine test cases to check if write errors are retried properly.
 * Looks through the write errors array and retries command against the recipient.
 */
function retryFailedWrites(primaryDB, collName, writeErrors, ops) {
    jsTestLog("Retrying writes that errored during migration.");

    writeErrors.forEach(err => {
        let retryOp = ops[0].operations[err.index];
        switch (ops[0].batchType) {
            case kBatchTypes.insert:
                assert.commandWorked(primaryDB[collName].insert(retryOp));
                break;
            case kBatchTypes.update:
                assert.commandWorked(primaryDB[collName].update(retryOp.q, retryOp.u));
                break;
            case kBatchTypes.remove:
                assert.commandWorked(primaryDB[collName].remove(retryOp));
                break;
            default:
                throw new Error(`Invalid write op type ${retryOp.batchType}.`);
        }
    });
}

(() => {
    jsTestLog("Testing unordered bulk insert against a tenant migration that commits.");

    donorRst.startSet();
    donorRst.initiate();

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        recipientRst.stopSet();
        return;
    }

    const tenantId = "bulkUnorderedInserts-committed";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    const bulkWriteThread =
        new Thread(bulkWriteDocsUnordered, primary.host, dbName, kCollName, kNumWriteOps);

    bulkWriteThread.start();
    writeFp.wait();

    const migrationRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assert.eq(migrationRes.state, TenantMigrationTest.State.kCommitted);

    writeFp.off();
    bulkWriteThread.join();

    const bulkWriteRes = bulkWriteThread.returnData();
    const writeErrors = bulkWriteRes.res.writeErrors;

    assert.eq(primaryDB[kCollName].count(), bulkWriteRes.res.nInserted);
    assert.eq(writeErrors.length,
              (kNumWriteOps - (kMaxBatchSize * kNumWriteBatchesWithoutMigrationConflict)));

    writeErrors.forEach((err, arrIndex) => {
        assert.eq(err.code, ErrorCodes.TenantMigrationCommitted);
        if (arrIndex == 0) {
            assert(err.errmsg);
        } else {
            assert(!err.errmsg);
        }

        assert.eq(err.errInfo.recipientConnectionString,
                  tenantMigrationTest.getRecipientConnString());
        assert.eq(err.errInfo.tenantId, tenantId);
    });

    tenantMigrationTest.stop();
    donorRst.stopSet();
    recipientRst.stopSet();
})();

(() => {
    jsTestLog(
        "Testing unordered bulk insert against a tenant migration that blocks a few inserts and commits.");

    donorRst.startSet();
    donorRst.initiate();

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        recipientRst.stopSet();
        return;
    }

    const tenantId = "bulkUnorderedInserts-committed";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: kRecipientConnString,
        tenantId,
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    const bulkWriteThread =
        new Thread(bulkWriteDocsUnordered, primary.host, dbName, kCollName, kNumWriteOps);

    const blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationAfterBlockingStarts");
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);

    bulkWriteThread.start();
    writeFp.wait();

    migrationThread.start();
    blockFp.wait();

    writeFp.off();
    sleep(Math.random() * kMaxSleepTimeMS);
    blockFp.off();

    bulkWriteThread.join();
    migrationThread.join();

    const migrationRes = assert.commandWorked(migrationThread.returnData());
    assert.eq(migrationRes.state, TenantMigrationTest.State.kCommitted);

    let bulkWriteRes = bulkWriteThread.returnData();
    let writeErrors = bulkWriteRes.res.writeErrors;

    assert.eq(primaryDB[kCollName].count(), bulkWriteRes.res.nInserted);
    assert.eq(writeErrors.length,
              (kNumWriteOps - (kMaxBatchSize * kNumWriteBatchesWithoutMigrationConflict)));

    writeErrors.forEach((err, index) => {
        assert.eq(err.code, ErrorCodes.TenantMigrationCommitted);
        if (index == 0) {
            assert.eq(err.errmsg, "Write must be re-routed to the new owner of this tenant");
        } else {
            assert.eq(err.errmsg, "");
        }

        assert.eq(err.errInfo.recipientConnectionString,
                  tenantMigrationTest.getRecipientConnString());
        assert.eq(err.errInfo.tenantId, tenantId);
    });

    tenantMigrationTest.stop();
    donorRst.stopSet();
    recipientRst.stopSet();
})();

(() => {
    jsTestLog("Testing unordered bulk insert against a tenant migration that aborts.");

    donorRst.startSet();
    donorRst.initiate();

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});

    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        recipientRst.stopSet();
        return;
    }
    const tenantId = "bulkUnorderedInserts-aborted";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId,
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    const bulkWriteThread =
        new Thread(bulkWriteDocsUnordered, primary.host, dbName, kCollName, kNumWriteOps);

    const abortFp = configureFailPoint(primaryDB, "abortTenantMigrationAfterBlockingStarts");

    // The failpoint below is used to ensure that a write to throw TenantMigrationConflict in the op
    // observer. Without this failpoint, the migration could have already aborted by the time the
    // write gets to the op observer.
    const blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationAfterBlockingStarts");
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);

    bulkWriteThread.start();
    writeFp.wait();

    migrationThread.start();
    blockFp.wait();

    writeFp.off();
    sleep(Math.random() * kMaxSleepTimeMS);
    blockFp.off();

    bulkWriteThread.join();
    migrationThread.join();

    abortFp.off();

    const migrationRes = assert.commandWorked(migrationThread.returnData());
    assert.eq(migrationRes.state, TenantMigrationTest.State.kAborted);

    const bulkWriteRes = bulkWriteThread.returnData();
    const writeErrors = bulkWriteRes.res.writeErrors;

    assert.eq(primaryDB[kCollName].count(), bulkWriteRes.res.nInserted);
    assert.eq(writeErrors.length,
              (kNumWriteOps - (kMaxBatchSize * kNumWriteBatchesWithoutMigrationConflict)));

    writeErrors.forEach((err, arrIndex) => {
        assert.eq(err.code, ErrorCodes.TenantMigrationAborted);
        if (arrIndex == 0) {
            assert(err.errmsg);
        } else {
            assert(!err.errmsg);
        }
    });

    tenantMigrationTest.stop();
    donorRst.stopSet();
    recipientRst.stopSet();
})();

(() => {
    jsTestLog("Testing ordered bulk inserts against a tenant migration that commits.");

    donorRst.startSet();
    donorRst.initiate();

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        recipientRst.stopSet();
        return;
    }

    const tenantId = "bulkOrderedInserts-committed";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    const bulkWriteThread =
        new Thread(bulkWriteDocsOrdered, primary.host, dbName, kCollName, kNumWriteOps);

    bulkWriteThread.start();
    writeFp.wait();

    const migrationRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assert.eq(migrationRes.state, TenantMigrationTest.State.kCommitted);

    writeFp.off();
    bulkWriteThread.join();

    const bulkWriteRes = bulkWriteThread.returnData();
    const writeErrors = bulkWriteRes.res.writeErrors;

    assert.eq(primaryDB[kCollName].count(), bulkWriteRes.res.nInserted);
    assert.eq(writeErrors.length, 1);
    assert(writeErrors[0].errmsg);

    // The single write error should correspond to the first write after the migration started
    // blocking writes.
    assert.eq(writeErrors[0].index, kNumWriteBatchesWithoutMigrationConflict * kMaxBatchSize);
    assert.eq(writeErrors[0].code, ErrorCodes.TenantMigrationCommitted);
    assert.eq(writeErrors[0].errInfo.recipientConnectionString,
              tenantMigrationTest.getRecipientConnString());
    assert.eq(writeErrors[0].errInfo.tenantId, tenantId);

    tenantMigrationTest.stop();
    donorRst.stopSet();
    recipientRst.stopSet();
})();

(() => {
    jsTestLog(
        "Testing ordered bulk insert against a tenant migration that blocks a few inserts and commits.");

    donorRst.startSet();
    donorRst.initiate();

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        recipientRst.stopSet();
        return;
    }

    const tenantId = "bulkOrderedInserts-committed";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId,
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    const bulkWriteThread =
        new Thread(bulkWriteDocsOrdered, primary.host, dbName, kCollName, kNumWriteOps);

    const blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationAfterBlockingStarts");
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);

    bulkWriteThread.start();
    writeFp.wait();

    migrationThread.start();
    blockFp.wait();

    writeFp.off();
    sleep(Math.random() * kMaxSleepTimeMS);
    blockFp.off();

    bulkWriteThread.join();
    migrationThread.join();

    const migrationRes = assert.commandWorked(migrationThread.returnData());
    assert.eq(migrationRes.state, TenantMigrationTest.State.kCommitted);

    const bulkWriteRes = bulkWriteThread.returnData();
    const writeErrors = bulkWriteRes.res.writeErrors;

    assert.eq(primaryDB[kCollName].count(), bulkWriteRes.res.nInserted);
    assert.eq(writeErrors.length, 1);
    assert(writeErrors[0].errmsg);

    // The single write error should correspond to the first write after the migration started
    // blocking writes.
    assert.eq(writeErrors[0].index, kNumWriteBatchesWithoutMigrationConflict * kMaxBatchSize);
    assert.eq(writeErrors[0].code, ErrorCodes.TenantMigrationCommitted);
    assert.eq(writeErrors[0].errInfo.recipientConnectionString,
              tenantMigrationTest.getRecipientConnString());
    assert.eq(writeErrors[0].errInfo.tenantId, tenantId);

    tenantMigrationTest.stop();
    donorRst.stopSet();
    recipientRst.stopSet();
})();

(() => {
    jsTestLog("Testing ordered bulk write against a tenant migration that aborts.");

    donorRst.startSet();
    donorRst.initiate();

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        recipientRst.stopSet();
        return;
    }

    const tenantId = "bulkOrderedInserts-aborted";
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId,
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const dbName = tenantMigrationTest.tenantDB(tenantId, kTenantDefinedDbName);
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));

    const writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    const bulkWriteThread =
        new Thread(bulkWriteDocsOrdered, primary.host, dbName, kCollName, kNumWriteOps);

    const abortFp = configureFailPoint(primaryDB, "abortTenantMigrationAfterBlockingStarts");

    // The failpoint below is used to ensure that a write to throw TenantMigrationConflict in the op
    // observer. Without this failpoint, the migration could have already aborted by the time the
    // write gets to the op observer.
    const blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationAfterBlockingStarts");
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);

    bulkWriteThread.start();
    writeFp.wait();

    migrationThread.start();
    blockFp.wait();

    writeFp.off();
    sleep(Math.random() * kMaxSleepTimeMS);
    blockFp.off();

    bulkWriteThread.join();
    migrationThread.join();

    abortFp.off();

    const migrationRes = assert.commandWorked(migrationThread.returnData());
    assert.eq(migrationRes.state, TenantMigrationTest.State.kAborted);

    const bulkWriteRes = bulkWriteThread.returnData();
    const writeErrors = bulkWriteRes.res.writeErrors;

    assert.eq(primaryDB[kCollName].count(), bulkWriteRes.res.nInserted);
    assert.eq(writeErrors.length, 1);
    assert(writeErrors[0].errmsg);

    // The single write error should correspond to the first write after the migration started
    // blocking writes.
    assert.eq(writeErrors[0].index, kNumWriteBatchesWithoutMigrationConflict * kMaxBatchSize);
    assert.eq(writeErrors[0].code, ErrorCodes.TenantMigrationAborted);

    tenantMigrationTest.stop();
    donorRst.stopSet();
    recipientRst.stopSet();
})();
})();
