/**
 * Tests read and write access after a migration aborted and also test read and write after a
 * migration commmitted successfully.
 * @tags: [requires_fcv_52]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

function donorStartMigrationCmd(tenantID, realConnUrl) {
    return {
        donorStartMigration: 1,
        tenantId: tenantID.str,
        migrationId: UUID(),
        recipientConnectionString: realConnUrl,
        readPreference: {mode: "primary"}
    };
}

/*
 * Test running a migration, abort it and then try to insert data after migration completes.
 * This should succeed since the migration did not happen and the owner of the data
 * stays the same.
 */
function insertAndCountAfterMigrationAborted(st) {
    const tenantID = ObjectId();
    var kDbName = tenantID.str + "_test1";

    assert.commandWorked(st.s0.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);

    configureFailPoint(st.rs0.getPrimary().getDB('admin'),
                       "abortTenantMigrationBeforeLeavingBlockingState");

    let cmdObj = donorStartMigrationCmd(tenantID, st.rs1.getURL());

    assert.soon(function() {
        let res = assert.commandWorked(st.rs0.getPrimary().getDB('admin').runCommand(cmdObj));
        return res['state'] == "aborted";
    }, "migration not in aborted state", 1 * 10000, 1 * 1000);

    let db = st.s0.getDB(kDbName);
    assert.commandWorked(db.bar.insert([{n: 1}, {n: 2}, {n: 3}]));
    assert.eq(1, db.bar.find({n: 1}).count());
}

/*
 * Test running a migration and then try to insert data after migration completes.
 * This should fail since the insert would return a "committed" error
 * because the migration finished and the owner of the data changed.
 */
function insertAndCountAfterMigrationCommitted(st) {
    const tenantID = ObjectId();
    var kDbName = tenantID.str + "_test2";

    assert.commandWorked(st.s0.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);

    let cmdObj = donorStartMigrationCmd(tenantID, st.rs1.getURL());

    assert.soon(function() {
        let res = assert.commandWorked(st.rs0.getPrimary().getDB('admin').runCommand(cmdObj));
        return res['state'] == "committed";
    }, "migration not in committed state", 1 * 10000, 1 * 1000);

    let db = st.s0.getDB(kDbName);
    assert.commandFailedWithCode(db.runCommand({count: "bar"}),
                                 ErrorCodes.TenantMigrationCommitted);
    assert.commandFailedWithCode(db.bar.insert([{n: 1}, {n: 2}, {n: 3}]),
                                 ErrorCodes.TenantMigrationCommitted);
}

function bulkInsertDocsOrdered(primaryHost, dbName, collName, numDocs) {
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

/*
 * Test running a migration and then try to do an unordered bulk insert while the migration is in a
 * blocking state before it aborts. Since mongoq retries internally on TenantMigrationAborted the
 * test should pass with no error since the writes are retried.
 */
function orderedBulkInsertDuringBlockingState(st) {
    jsTest.log("Starting test - ordered bulk insert during migration blocking state then aborts.");
    const tenantID = ObjectId();
    const kDbName = tenantID.str + "_test";
    let adminDB = st.rs0.getPrimary().getDB('admin');

    const kCollName = 'foo';

    assert.commandWorked(st.s0.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);

    let blockingFp = configureFailPoint(adminDB, "pauseTenantMigrationBeforeLeavingBlockingState");

    let abortFailPoint =
        configureFailPoint(adminDB, "abortTenantMigrationBeforeLeavingBlockingState");

    let cmdObj = donorStartMigrationCmd(tenantID, st.rs1.getURL());
    assert.commandWorked(adminDB.runCommand(cmdObj));
    blockingFp.wait();

    const kNumWriteOps = 6;
    const bulkWriteThread =
        new Thread((bulkInsertDocsOrderedFunc, primaryHost, dbName, collName, numDocs) => {
            const res = bulkInsertDocsOrderedFunc(primaryHost, dbName, collName, numDocs);
            assert.eq(res.res.nInserted, numDocs);
            assert.eq(res.res.writeErrors.length, 0);
        }, bulkInsertDocsOrdered, st.s0.host, kDbName, kCollName, kNumWriteOps);
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

let st = new ShardingTest({
    shards: 2,
    mongosOptions: {setParameter: {tenantMigrationDisableX509Auth: true}},
    shardOptions: {setParameter: {tenantMigrationDisableX509Auth: true}}
});

insertAndCountAfterMigrationCommitted(st);
insertAndCountAfterMigrationAborted(st);
orderedBulkInsertDuringBlockingState(st);

st.stop();
})();
