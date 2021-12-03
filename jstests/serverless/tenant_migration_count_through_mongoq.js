/**
 * Tests read and write access after a migration aborted and also test read and write after a
 * migration commmitted successfully.
 * @tags: [requires_fcv_52]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/serverless/serverlesstest.js");

function donorStartMigrationCmd(tenantID, realConnUrl) {
    return {
        donorStartMigration: 1,
        tenantId: tenantID.str,
        migrationId: UUID(),
        recipientConnectionString: realConnUrl,
        readPreference: {mode: "primary"}
    };
}

let st = new ServerlessTest();

/*
 * Test running a migration and then try to insert data after migration completes.
 * This should fail since the insert would return a "committed" error
 * because the migration finished and the owner of the data changed.
 */
(() => {
    const tenantID = ObjectId();
    var kDbName = tenantID.str + "_test2";

    assert.commandWorked(st.q0.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);

    let cmdObj = donorStartMigrationCmd(tenantID, st.rs1.getURL());

    assert.soon(function() {
        let res = assert.commandWorked(st.rs0.getPrimary().getDB('admin').runCommand(cmdObj));
        return res['state'] == "committed";
    }, "migration not in committed state", 1 * 10000, 1 * 1000);

    let db = st.q0.getDB(kDbName);
    assert.commandFailedWithCode(db.runCommand({count: "bar"}),
                                 ErrorCodes.TenantMigrationCommitted);
})();

/*
 * Test running a migration, abort it and then try to insert data after migration completes.
 * This should succeed since the migration did not happen and the owner of the data
 * stays the same.
 */
(() => {
    const tenantID = ObjectId();
    var kDbName = tenantID.str + "_test1";

    assert.commandWorked(st.q0.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, st.shard0.shardName);
    let db = st.q0.getDB(kDbName);
    assert.commandWorked(db.bar.insert([{n: 1}, {n: 2}, {n: 3}]));

    configureFailPoint(st.rs0.getPrimary().getDB('admin'),
                       "abortTenantMigrationBeforeLeavingBlockingState");

    let cmdObj = donorStartMigrationCmd(tenantID, st.rs1.getURL());

    assert.soon(function() {
        let res = assert.commandWorked(st.rs0.getPrimary().getDB('admin').runCommand(cmdObj));
        return res['state'] == "aborted";
    }, "migration not in aborted state", 1 * 10000, 1 * 1000);

    assert.eq(1, db.bar.find({n: 1}).count());
})();

st.stop();
})();
