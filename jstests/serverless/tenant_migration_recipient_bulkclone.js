/**
 * Tests the tenant migration recipient creates collections during the bulk clone.
 *
 * @tags: [requires_fcv_52, serverless]
 */

(function() {
"use strict";

load("jstests/serverless/serverlesstest.js");

let st = new ServerlessTest();

let donor = st.rs0;
let recipient = st.rs1;
let mongos = st.q0;
let adminDB = donor.getPrimary().getDB('admin');

jsTest.log(
    "Starting test a tenant migration succeeds when there is pre-existing data for the tenant on the donor.");
const kTenantID = ObjectId();
const kDbName = kTenantID.str + "_testDB";
let db = mongos.getDB(kDbName);

// Ensure the donor is the primary shard so that it has the database of the tenant.
assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, st.shard0.shardName);

jsTest.log("Going to create data for the tenant.");
assert.commandWorked(db.foo.insert([{_id: 10, x: 1}, {_id: 20, y: 1}, {_id: 30, z: 1}]));

// Run donorStartMigration command to start migration and poll the migration status with the
// same command object.
jsTest.log("Going to start tenant migration.");
let startMigrationCmd = {
    donorStartMigration: 1,
    tenantId: kTenantID.str,
    migrationId: UUID(),
    recipientConnectionString: recipient.getURL(),
    readPreference: {mode: "primary"}
};
assert.soon(function() {
    let res = assert.commandWorked(adminDB.runCommand(startMigrationCmd));
    return res['state'] == "committed";
}, "migration not in committed state", 1 * 10000, 1 * 1000);

jsTest.log("Going to verify the documents on recipient.");
let targetDB = recipient.getPrimary().getDB(kDbName);
assert.eq(3, targetDB.foo.find().itcount());
assert.eq({_id: 10, x: 1}, targetDB.foo.findOne({_id: 10}));
assert.eq({_id: 20, y: 1}, targetDB.foo.findOne({_id: 20}));
assert.eq({_id: 30, z: 1}, targetDB.foo.findOne({_id: 30}));

// Enable stale reads on the donor set so that end of test data consistency check can pass.
donor.nodes.forEach(
    node => assert.commandWorked(node.adminCommand(
        {configureFailPoint: "tenantMigrationDonorAllowsNonTimestampedReads", mode: "alwaysOn"})));
st.stop();
}());
