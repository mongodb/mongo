/**
 * Tests findAndModify returns the expected tenant migration error or succeeds when sent through
 * mongos after a tenant migration commits or aborts.
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

const kCollName = 'foo';

let findAndModifyCmd = {findAndModify: kCollName, update: {$set: {y: 1}}, upsert: true};

let st = new ShardingTest({
    shards: 2,
    mongosOptions: {setParameter: {tenantMigrationDisableX509Auth: true}},
    shardOptions: {setParameter: {tenantMigrationDisableX509Auth: true}}
});

let adminDB = st.rs0.getPrimary().getDB('admin');

(() => {
    jsTest.log("Starting test calling findAndModify after the migration has committed.");
    const tenantID = ObjectId();
    const kDbName = tenantID.str + "_test";
    let db = st.s0.getDB(kDbName);

    assert.commandWorked(st.s0.adminCommand({enableSharding: kDbName}));
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

    assert.commandWorked(st.s0.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);

    configureFailPoint(adminDB, "abortTenantMigrationBeforeLeavingBlockingState");

    let cmdObj = donorStartMigrationCmd(tenantID, st.rs1.getURL());

    assert.soon(function() {
        let res = assert.commandWorked(adminDB.runCommand(cmdObj));
        return res['state'] == "aborted";
    }, "migration not in aborted state", 1 * 10000, 1 * 1000);

    assert.commandWorked(st.s0.getDB(kDbName).runCommand(findAndModifyCmd));
})();

(() => {
    jsTest.log("Starting test calling findAndModify during migration blocking state then aborts.");
    const tenantID = ObjectId();
    const kDbName = tenantID.str + "_test";

    assert.commandWorked(st.s0.adminCommand({enableSharding: kDbName}));
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
    }, st.s0.host, kDbName, findAndModifyCmd);
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

st.stop();
})();
