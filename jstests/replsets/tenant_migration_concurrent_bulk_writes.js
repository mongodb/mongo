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
            enableTenantMigrations: true,
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
            enableTenantMigrations: true,
            internalInsertMaxBatchSize:
                kMaxBatchSize /* Decrease internal max batch size so we can still show writes are
                                 batched without inserting hundreds of documents. */
            ,
            // TODO SERVER-51734: Remove the failpoint 'returnResponseOkForRecipientSyncDataCmd'.
            'failpoint.returnResponseOkForRecipientSyncDataCmd': tojson({mode: 'alwaysOn'})
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

    let dbName = "bulkUnorderedInserts-committed_" + kTenantDefinedDbName;
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: kRecipientConnString,
        tenantId: tenantId,
        readPreference: {mode: "primary"},
    };

    let writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    let bulkWriteThread =
        new Thread(bulkWriteDocsUnordered, primary.host, dbName, kCollName, kNumWriteOps);

    bulkWriteThread.start();
    writeFp.wait();

    let migrationRes =
        assert.commandWorked(TenantMigrationUtil.startMigration(primary.host, migrationOpts));
    assert.eq(migrationRes.state, "committed");

    writeFp.off();
    bulkWriteThread.join();

    let bulkWriteRes = bulkWriteThread.returnData();
    let writeErrors = bulkWriteRes.res.writeErrors;

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

        assert.eq(err.errInfo.recipientConnectionString, migrationOpts.recipientConnString);
        assert.eq(err.errInfo.tenantId, migrationOpts.tenantId);
    });

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

    let dbName = "bulkUnorderedInserts-committed_" + kTenantDefinedDbName;
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: kRecipientConnString,
        tenantId: tenantId,
        readPreference: {mode: "primary"},
    };

    let writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    let bulkWriteThread =
        new Thread(bulkWriteDocsUnordered, primary.host, dbName, kCollName, kNumWriteOps);

    let blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationAfterBlockingStarts");
    let migrationThread =
        new Thread(TenantMigrationUtil.startMigration, primary.host, migrationOpts);

    bulkWriteThread.start();
    writeFp.wait();

    migrationThread.start();
    blockFp.wait();

    writeFp.off();
    sleep(Math.random() * kMaxSleepTimeMS);
    blockFp.off();

    bulkWriteThread.join();
    migrationThread.join();

    let migrationRes = assert.commandWorked(migrationThread.returnData());
    assert.eq(migrationRes.state, "committed");

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

        assert.eq(err.errInfo.recipientConnectionString, migrationOpts.recipientConnString);
        assert.eq(err.errInfo.tenantId, migrationOpts.tenantId);
    });

    donorRst.stopSet();
    recipientRst.stopSet();
})();

(() => {
    jsTestLog("Testing unordered bulk insert against a tenant migration that aborts.");

    donorRst.startSet();
    donorRst.initiate();
    recipientRst.startSet();
    recipientRst.initiate();

    let dbName = "bulkUnorderedInserts-aborted_" + kTenantDefinedDbName;
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: kRecipientConnString,
        tenantId: tenantId,
        readPreference: {mode: "primary"},
    };

    let writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    let bulkWriteThread =
        new Thread(bulkWriteDocsUnordered, primary.host, dbName, kCollName, kNumWriteOps);

    let abortFp = configureFailPoint(primaryDB, "abortTenantMigrationAfterBlockingStarts");

    // The failpoint below is used to ensure that a write to throw TenantMigrationConflict in the op
    // observer. Without this failpoint, the migration could have already aborted by the time the
    // write gets to the op observer.
    let blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationAfterBlockingStarts");
    let migrationThread =
        new Thread(TenantMigrationUtil.startMigration, primary.host, migrationOpts);

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

    let migrationRes = assert.commandWorked(migrationThread.returnData());
    assert.eq(migrationRes.state, "aborted");

    let bulkWriteRes = bulkWriteThread.returnData();
    let writeErrors = bulkWriteRes.res.writeErrors;

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

    donorRst.stopSet();
    recipientRst.stopSet();
})();

(() => {
    jsTestLog("Testing ordered bulk inserts against a tenant migration that commits.");

    donorRst.startSet();
    donorRst.initiate();
    recipientRst.startSet();
    recipientRst.initiate();

    let dbName = "bulkOrderedInserts-committed_" + kTenantDefinedDbName;
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: kRecipientConnString,
        tenantId: tenantId,
        readPreference: {mode: "primary"},
    };

    let writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    let bulkWriteThread =
        new Thread(bulkWriteDocsOrdered, primary.host, dbName, kCollName, kNumWriteOps);

    bulkWriteThread.start();
    writeFp.wait();

    let migrationRes =
        assert.commandWorked(TenantMigrationUtil.startMigration(primary.host, migrationOpts));
    assert.eq(migrationRes.state, "committed");

    writeFp.off();
    bulkWriteThread.join();

    let bulkWriteRes = bulkWriteThread.returnData();
    let writeErrors = bulkWriteRes.res.writeErrors;

    assert.eq(primaryDB[kCollName].count(), bulkWriteRes.res.nInserted);
    assert.eq(writeErrors.length, 1);
    assert(writeErrors[0].errmsg);

    // The single write error should correspond to the first write after the migration started
    // blocking writes.
    assert.eq(writeErrors[0].index, kNumWriteBatchesWithoutMigrationConflict * kMaxBatchSize);
    assert.eq(writeErrors[0].code, ErrorCodes.TenantMigrationCommitted);
    assert.eq(writeErrors[0].errInfo.recipientConnectionString, migrationOpts.recipientConnString);
    assert.eq(writeErrors[0].errInfo.tenantId, migrationOpts.tenantId);

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

    let dbName = "bulkOrderedInserts-committed_" + kTenantDefinedDbName;
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: kRecipientConnString,
        tenantId: tenantId,
        readPreference: {mode: "primary"},
    };

    let writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    let bulkWriteThread =
        new Thread(bulkWriteDocsOrdered, primary.host, dbName, kCollName, kNumWriteOps);

    let blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationAfterBlockingStarts");
    let migrationThread =
        new Thread(TenantMigrationUtil.startMigration, primary.host, migrationOpts);

    bulkWriteThread.start();
    writeFp.wait();

    migrationThread.start();
    blockFp.wait();

    writeFp.off();
    sleep(Math.random() * kMaxSleepTimeMS);
    blockFp.off();

    bulkWriteThread.join();
    migrationThread.join();

    let migrationRes = assert.commandWorked(migrationThread.returnData());
    assert.eq(migrationRes.state, "committed");

    let bulkWriteRes = bulkWriteThread.returnData();
    let writeErrors = bulkWriteRes.res.writeErrors;

    assert.eq(primaryDB[kCollName].count(), bulkWriteRes.res.nInserted);
    assert.eq(writeErrors.length, 1);
    assert(writeErrors[0].errmsg);

    // The single write error should correspond to the first write after the migration started
    // blocking writes.
    assert.eq(writeErrors[0].index, kNumWriteBatchesWithoutMigrationConflict * kMaxBatchSize);
    assert.eq(writeErrors[0].code, ErrorCodes.TenantMigrationCommitted);
    assert.eq(writeErrors[0].errInfo.recipientConnectionString, migrationOpts.recipientConnString);
    assert.eq(writeErrors[0].errInfo.tenantId, migrationOpts.tenantId);

    donorRst.stopSet();
    recipientRst.stopSet();
})();

(() => {
    jsTestLog("Testing ordered bulk write against a tenant migration that aborts.");

    donorRst.startSet();
    donorRst.initiate();
    recipientRst.startSet();
    recipientRst.initiate();

    let dbName = "bulkOrderedInserts-aborted_" + kTenantDefinedDbName;
    const primary = donorRst.getPrimary();
    const primaryDB = primary.getDB(dbName);

    assert.commandWorked(primaryDB.runCommand({create: kCollName}));
    const tenantId = dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        recipientConnString: kRecipientConnString,
        tenantId: tenantId,
        readPreference: {mode: "primary"},
    };

    let writeFp = configureFailPoint(
        primaryDB, "hangDuringBatchInsert", {}, {skip: kNumWriteBatchesWithoutMigrationConflict});
    let bulkWriteThread =
        new Thread(bulkWriteDocsOrdered, primary.host, dbName, kCollName, kNumWriteOps);

    let abortFp = configureFailPoint(primaryDB, "abortTenantMigrationAfterBlockingStarts");

    // The failpoint below is used to ensure that a write to throw TenantMigrationConflict in the op
    // observer. Without this failpoint, the migration could have already aborted by the time the
    // write gets to the op observer.
    let blockFp = configureFailPoint(primaryDB, "pauseTenantMigrationAfterBlockingStarts");
    let migrationThread =
        new Thread(TenantMigrationUtil.startMigration, primary.host, migrationOpts);

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

    let migrationRes = assert.commandWorked(migrationThread.returnData());
    assert.eq(migrationRes.state, "aborted");

    let bulkWriteRes = bulkWriteThread.returnData();
    let writeErrors = bulkWriteRes.res.writeErrors;

    assert.eq(primaryDB[kCollName].count(), bulkWriteRes.res.nInserted);
    assert.eq(writeErrors.length, 1);
    assert(writeErrors[0].errmsg);

    // The single write error should correspond to the first write after the migration started
    // blocking writes.
    assert.eq(writeErrors[0].index, kNumWriteBatchesWithoutMigrationConflict * kMaxBatchSize);
    assert.eq(writeErrors[0].code, ErrorCodes.TenantMigrationAborted);

    donorRst.stopSet();
    recipientRst.stopSet();
})();
})();
