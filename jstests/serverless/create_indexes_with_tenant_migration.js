/**
 * Tests createIndexes returns the expected tenant migration error or succeeds when sent through
 * mongoq after a tenant migration commits or aborts.
 *
 * @tags: [requires_fcv_52, serverless]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/serverless/serverlesstest.js");
load('jstests/concurrency/fsm_libs/worker_thread.js');

// A function, not a constant, to ensure unique UUIDs.
function donorStartMigrationCmd(tenantID, realConnUrl) {
    return {
        donorStartMigration: 1,
        tenantId: tenantID.str,
        migrationId: UUID(),
        recipientConnectionString: realConnUrl,
        readPreference: {mode: "primary"}
    };
}

let createIndexesCmd = {createIndexes: "foo", indexes: [{key: {x: 1}, name: "x_1"}]};

let st = new ServerlessTest();
let donor = st.rs0;
let recipient = st.rs1;
let mongoq = st.q0;
let adminDB = donor.getPrimary().getDB('admin');

(() => {
    jsTest.log("Starting test calling createIndexes after the migration has committed.");
    const kTenantID = ObjectId();
    const kDbName = kTenantID.str + "_testDB";
    let db = mongoq.getDB(kDbName);

    assert.commandWorked(mongoq.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);

    // Run donorStartMigration command to start migration and poll the migration status with the
    // same command object.
    let startMigrationCmd = donorStartMigrationCmd(kTenantID, recipient.getURL());
    assert.soon(function() {
        let res = assert.commandWorked(adminDB.runCommand(startMigrationCmd));
        return res['state'] == "committed";
    }, "migration not in committed state", 1 * 10000, 1 * 1000);

    assert.commandFailedWithCode(db.runCommand(createIndexesCmd),
                                 ErrorCodes.TenantMigrationCommitted);
})();

(() => {
    jsTest.log("Starting test calling createIndexes after the migration has aborted.");
    const kTenantID = ObjectId();
    const kDbName = kTenantID.str + "_testDB";
    let db = mongoq.getDB(kDbName);

    assert.commandWorked(mongoq.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);

    let abortFailPoint =
        configureFailPoint(adminDB, "abortTenantMigrationBeforeLeavingBlockingState");

    let startMigrationCmd = donorStartMigrationCmd(kTenantID, recipient.getURL());
    assert.soon(function() {
        let res = assert.commandWorked(adminDB.runCommand(startMigrationCmd));
        return res['state'] == "aborted";
    }, "migration not in aborted state", 1 * 10000, 1 * 1000);

    assert.commandWorked(db.runCommand(createIndexesCmd));

    abortFailPoint.off();
})();

(() => {
    jsTest.log("Starting test calling createIndexes during migration blocking state then aborts.");
    const kTenantID = ObjectId();
    const kDbName = kTenantID.str + "_testDB";

    assert.commandWorked(mongoq.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);

    let blockingFailPoint =
        configureFailPoint(adminDB, "pauseTenantMigrationBeforeLeavingBlockingState");
    let abortFailPoint =
        configureFailPoint(adminDB, "abortTenantMigrationBeforeLeavingBlockingState");

    // Start the migration asynchronously and then immediately return the current state of the
    // migration.
    let startMigrationCmd = donorStartMigrationCmd(kTenantID, recipient.getURL());
    assert.commandWorked(adminDB.runCommand(startMigrationCmd));

    blockingFailPoint.wait();

    // Send createIndexes command to mongoq in an individual thread.
    let createIndexesThread = new Thread((mongoqConnString, kDbName, createIndexesCmd) => {
        let mongoqConn = new Mongo(mongoqConnString);
        // Expect to receive an ok response for the createIndexes command.
        assert.commandWorked(mongoqConn.getDB(kDbName).runCommand(createIndexesCmd));
    }, st.q0.host, kDbName, createIndexesCmd);
    createIndexesThread.start();

    // Poll the numBlockedWrites of tenant migration access blocker from donor and expect it's
    // increased by the blocked createIndexes command.
    assert.soon(function() {
        let mtab = donor.getPrimary()
                       .getDB('admin')
                       .adminCommand({serverStatus: 1})
                       .tenantMigrationAccessBlocker[kTenantID.str]
                       .donor;
        return mtab.numBlockedWrites > 0;
    }, "no blocked writes found", 1 * 10000, 1 * 1000);

    blockingFailPoint.off();

    // Expect to get aborted state when polling the migration state from donor.
    assert.soon(function() {
        let res = assert.commandWorked(adminDB.runCommand(startMigrationCmd));
        return res['state'] == "aborted";
    }, "migration not in aborted state", 1 * 10000, 1 * 1000);

    createIndexesThread.join();

    abortFailPoint.off();
})();

st.stop();
})();
