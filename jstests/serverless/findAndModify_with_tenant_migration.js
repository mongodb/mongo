/**
 * Tests findAndModify returns the expected tenant migration error or succeeds when sent through
 * mongos after a tenant migration commits or aborts.
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

const kCollName = 'foo';

let findAndModifyCmd = {findAndModify: kCollName, update: {$set: {y: 1}}, upsert: true};

let st = new ServerlessTest();

let adminDB = st.rs0.getPrimary().getDB('admin');

(() => {
    jsTest.log("Starting test calling findAndModify after the migration has committed.");
    const tenantID = ObjectId();
    const kDbName = tenantID.str + "_test";
    let db = st.q0.getDB(kDbName);

    assert.commandWorked(st.q0.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);

    let cmdObj = donorStartMigrationCmd(tenantID, st.rs1.getURL());

    assert.soon(function() {
        let res = assert.commandWorked(adminDB.runCommand(cmdObj));
        return res['state'] == "committed";
    }, "migration not in committed state", 1 * 10000, 1 * 1000);

    assert.commandFailedWithCode(db.runCommand(findAndModifyCmd),
                                 ErrorCodes.TenantMigrationCommitted);
})();

(() => {
    jsTest.log("Starting test calling findAndModify after the migration has aborted.");
    const tenantID = ObjectId();
    const kDbName = tenantID.str + "_test";

    assert.commandWorked(st.q0.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);

    configureFailPoint(adminDB, "abortTenantMigrationBeforeLeavingBlockingState");

    let cmdObj = donorStartMigrationCmd(tenantID, st.rs1.getURL());

    assert.soon(function() {
        let res = assert.commandWorked(adminDB.runCommand(cmdObj));
        return res['state'] == "aborted";
    }, "migration not in aborted state", 1 * 10000, 1 * 1000);

    assert.commandWorked(st.q0.getDB(kDbName).runCommand(findAndModifyCmd));
})();

(() => {
    jsTest.log("Starting test calling findAndModify during migration blocking state then aborts.");
    const tenantID = ObjectId();
    const kDbName = tenantID.str + "_test";

    assert.commandWorked(st.q0.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);

    let blockingFp = configureFailPoint(adminDB, "pauseTenantMigrationBeforeLeavingBlockingState");

    let abortFailPoint =
        configureFailPoint(adminDB, "abortTenantMigrationBeforeLeavingBlockingState");

    let cmdObj = donorStartMigrationCmd(tenantID, st.rs1.getURL());
    assert.commandWorked(adminDB.runCommand(cmdObj));
    blockingFp.wait();

    let findAndModifyThread = new Thread((mongosConnString, dbName, findAndModifyCmd) => {
        let mongos = new Mongo(mongosConnString);
        assert.commandWorked(mongos.getDB(dbName).runCommand(findAndModifyCmd));
    }, st.q0.host, kDbName, findAndModifyCmd);
    findAndModifyThread.start();

    assert.soon(function() {
        let mtab = st.rs0.getPrimary()
                       .getDB('admin')
                       .adminCommand({serverStatus: 1})
                       .tenantMigrationAccessBlocker[tenantID.str]
                       .donor;
        return mtab.numBlockedWrites > 0;
    }, "no blocked writes found", 1 * 10000, 1 * 1000);

    blockingFp.off();
    abortFailPoint.off();

    assert.soon(function() {
        let res = assert.commandWorked(adminDB.runCommand(cmdObj));
        return res['state'] == "aborted";
    }, "migration not in aborted state", 1 * 10000, 1 * 1000);

    findAndModifyThread.join();
})();

(() => {
    jsTest.log(
        "Starting test findAndModify transaction during migration blocking state then aborts.");

    const tenantID = ObjectId();
    const kDbName = tenantID.str + "_test";
    let db = st.q0.getDB(kDbName);
    assert.commandWorked(db.foo.insert({'mydata': 1}));

    assert.commandWorked(st.q0.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);

    let blockingFp = configureFailPoint(adminDB, "pauseTenantMigrationBeforeLeavingBlockingState");
    let abortFailPoint =
        configureFailPoint(adminDB, "abortTenantMigrationBeforeLeavingBlockingState");

    let cmdObj = donorStartMigrationCmd(tenantID, st.rs1.getURL());
    assert.commandWorked(adminDB.runCommand(cmdObj));
    blockingFp.wait();

    let transactionThread = new Thread((mongosConnString, dbName) => {
        let mongos = new Mongo(mongosConnString);
        let session = mongos.startSession();
        let sessionDB = session.getDatabase(dbName);

        session.startTransaction();
        sessionDB.foo.findAndModify(
            {query: {'mydata': 1}, update: {$set: {'mydata': 2}}, new: true});

        let res = session.commitTransaction_forTesting();
        let assertErrorResponse = (res, expectedError, expectedErrLabel) => {
            jsTest.log("Going to check the response " + tojson(res));
            assert.commandFailedWithCode(res, expectedError, tojson(res));
            assert(res["errorLabels"] != null, "Error labels are absent from " + tojson(res));
            const expectedErrorLabels = [expectedErrLabel];
            assert.sameMembers(res["errorLabels"],
                               expectedErrorLabels,
                               "Error labels " + tojson(res["errorLabels"]) +
                                   " are different from expected " + expectedErrorLabels);
        };
        assertErrorResponse(res, ErrorCodes.TenantMigrationAborted, 'TransientTransactionError');

        jsTest.log("Going to retry commit transaction after tenant migration aborted.");
        res = session.commitTransaction_forTesting();
        assertErrorResponse(res, ErrorCodes.NoSuchTransaction, 'TransientTransactionError');
    }, st.q0.host, kDbName);
    transactionThread.start();

    assert.soon(function() {
        let mtab = st.rs0.getPrimary()
                       .getDB('admin')
                       .adminCommand({serverStatus: 1})
                       .tenantMigrationAccessBlocker[tenantID.str]
                       .donor;
        return mtab.numBlockedWrites > 0;
    }, "no blocked writes found", 1 * 10000, 1 * 1000);

    blockingFp.off();
    transactionThread.join();
    abortFailPoint.off();
})();

st.stop();
})();
