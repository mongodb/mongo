/**
 * Tests read and write access after a migration aborted and also test read and write after a
 * migration commmitted successfully.
 * @tags: [requires_fcv_52, serverless]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/serverless/serverlesstest.js");
load('jstests/concurrency/fsm_libs/worker_thread.js');

function donorStartMigrationCmd(tenantID, realConnUrl) {
    return {
        donorStartMigration: 1,
        tenantId: tenantID.str,
        migrationId: UUID(),
        recipientConnectionString: realConnUrl,
        readPreference: {mode: "primary"}
    };
}

function bulkInsertDocs(primaryHost, dbName, collName, numDocs, isOrdered) {
    const primary = new Mongo(primaryHost);
    let primaryDB = primary.getDB(dbName);
    let bulk = isOrdered ? primaryDB[collName].initializeOrderedBulkOp()
                         : primaryDB[collName].initializeUnorderedBulkOp();
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

/*
 * Test running a migration and then try to do an unordered/ordered bulk insert while the migration
 * is in a blocking state before it aborts. Since mongoq retries internally on
 * TenantMigrationAborted the test should pass with no error since the writes are retried.
 */
function orderedBulkInsertDuringBlockingState(st, isBulkWriteOrdered) {
    let titleOrderedStr = "Starting test - " + (isBulkWriteOrdered ? "ordered" : "unordered");
    jsTest.log(titleOrderedStr + " bulk insert during migration blocking state then aborts.");
    const tenantID = ObjectId();
    const kDbName = tenantID.str + "_test";
    let adminDB = st.rs0.getPrimary().getDB('admin');

    const kCollName = 'foo';

    assert.commandWorked(st.q0.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);

    let blockingFp = configureFailPoint(adminDB, "pauseTenantMigrationBeforeLeavingBlockingState");

    let abortFailPoint =
        configureFailPoint(adminDB, "abortTenantMigrationBeforeLeavingBlockingState");

    let cmdObj = donorStartMigrationCmd(tenantID, st.rs1.getURL());
    assert.commandWorked(adminDB.runCommand(cmdObj));
    blockingFp.wait();

    const kNumWriteOps = 6;
    const bulkWriteThread = new Thread(
        (bulkInsertDocsOrderedFunc, primaryHost, dbName, collName, numDocs, isOrdered) => {
            const res =
                bulkInsertDocsOrderedFunc(primaryHost, dbName, collName, numDocs, isOrdered);
            assert.eq(res.res.nInserted, numDocs);
            assert.eq(res.res.writeErrors.length, 0);
        },
        bulkInsertDocs,
        st.q0.host,
        kDbName,
        kCollName,
        kNumWriteOps,
        isBulkWriteOrdered);
    bulkWriteThread.start();

    assert.soon(function() {
        let mtab = st.rs0.getPrimary()
                       .getDB('admin')
                       .adminCommand({serverStatus: 1})
                       .tenantMigrationAccessBlocker[tenantID.str]
                       .donor;
        return mtab.numBlockedWrites > 0;
    }, "no blocked writes found", 1 * 5000, 1 * 1000);

    blockingFp.off();
    abortFailPoint.off();

    assert.soon(function() {
        let res = assert.commandWorked(adminDB.runCommand(cmdObj));
        return res['state'] == "aborted";
    }, "migration not in aborted state", 1 * 10000, 1 * 1000);

    bulkWriteThread.join();
}

/*
 * Test running a migration and then try to do an unordered/ordered once the migration has aborted.
 * Since the migration did not happen and was aborted, the bulk insert should succeed.
 */
function orderedBulkInsertAfterTenantMigrationAborted(st, isBulkWriteOrdered) {
    let titleOrderedStr = "Starting test - " + (isBulkWriteOrdered ? "ordered" : "unordered");
    jsTest.log(titleOrderedStr + " bulk insert after the migration has aborted.");
    const tenantID = ObjectId();
    const kDbName = tenantID.str + "_test";
    let adminDB = st.rs0.getPrimary().getDB('admin');

    const kCollName = 'foo';

    assert.commandWorked(st.q0.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);

    configureFailPoint(adminDB, "abortTenantMigrationBeforeLeavingBlockingState");

    let cmdObj = donorStartMigrationCmd(tenantID, st.rs1.getURL());
    assert.commandWorked(adminDB.runCommand(cmdObj));

    assert.soon(function() {
        let res = assert.commandWorked(adminDB.runCommand(cmdObj));
        return res['state'] == "aborted";
    }, "migration not in aborted state", 1 * 10000, 1 * 1000);

    const kNumWriteOps = 6;
    const bulkRes =
        bulkInsertDocs(st.q0.host, kDbName, kCollName, kNumWriteOps, isBulkWriteOrdered);
    assert.eq(bulkRes.res.nInserted, kNumWriteOps);
    assert.eq(bulkRes.res.writeErrors.length, 0);
}

let st = new ServerlessTest();

orderedBulkInsertDuringBlockingState(st, true);
orderedBulkInsertDuringBlockingState(st, false);
orderedBulkInsertAfterTenantMigrationAborted(st, true);
orderedBulkInsertAfterTenantMigrationAborted(st, false);

st.stop();
})();
