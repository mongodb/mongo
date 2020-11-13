/**
 * Shows that a tenant migration donor and recipient
 * are able to use their cluster certificates (normally used to talk to their own
 * replica set nodes) to talk to each other.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load('jstests/ssl/libs/ssl_helpers.js');

const donorRst = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    name: "donor",
    nodeOptions: {
        setParameter: {
            enableTenantMigrations: true,

            // Set the delay before a donor state doc is garbage collected to be short to speed up
            // the test.
            tenantMigrationGarbageCollectionDelayMS: 3 * 1000,

            // Set the TTL monitor to run at a smaller interval to speed up the test.
            ttlMonitorSleepSecs: 1,
        }
    }
});
const recipientRst = new ReplSetTest(
    {nodes: 1, name: "recipient", nodeOptions: {setParameter: {enableTenantMigrations: true}}});

donorRst.startSet({
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/server.pem",
    sslCAFile: "jstests/libs/ca.pem",
    sslClusterFile: "jstests/libs/client.pem",
    sslAllowInvalidHostnames: "",
});
donorRst.initiate();

recipientRst.startSet({
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/server.pem",
    sslCAFile: "jstests/libs/ca.pem",
    sslClusterFile: "jstests/libs/client.pem",
    sslAllowInvalidHostnames: "",
});
recipientRst.initiate();

const donorPrimary = donorRst.getPrimary();
const recipientPrimary = recipientRst.getPrimary();
const kRecipientConnString = recipientRst.getURL();

const kTenantId = "testDb";
const kConfigDonorsNS = "config.tenantMigrationDonors";

let configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);
configDonorsColl.createIndex({expireAt: 1}, {expireAfterSeconds: 0});

jsTest.log("Test the case where the migration commits");
const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    recipientConnString: recipientRst.getURL(),
    tenantId: kTenantId,
    readPreference: {mode: "primary"},
};

const res =
    assert.commandWorked(TenantMigrationUtil.startMigration(donorPrimary.host, migrationOpts));
assert.eq(res.state, "committed");

let donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
let commitOplogEntry =
    donorPrimary.getDB("local").oplog.rs.findOne({ns: kConfigDonorsNS, op: "u", o: donorDoc});
assert.eq(donorDoc.state, "committed");
assert.eq(donorDoc.commitOrAbortOpTime.ts, commitOplogEntry.ts);

let mtab;
assert.soon(() => {
    mtab = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
    return mtab[kTenantId].access === TenantMigrationUtil.accessState.kReject;
});
assert(mtab[kTenantId].commitOrAbortOpTime);

// Runs the donorForgetMigration command and asserts that the TenantMigrationAccessBlocker and donor
// state document are eventually removed from the donor.

jsTest.log("Test donorForgetMigration after the migration completes");

assert.commandWorked(
    donorPrimary.adminCommand({donorForgetMigration: 1, migrationId: migrationId}));

const recipientForgetMigrationMetrics =
    recipientPrimary.adminCommand({serverStatus: 1}).metrics.commands.recipientForgetMigration;
assert.eq(recipientForgetMigrationMetrics.failed, 0);

donorRst.nodes.forEach((node) => {
    assert.soon(() => null == node.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker);
});

assert.soon(() => 0 === donorPrimary.getCollection(kConfigDonorsNS).count({tenantId: kTenantId}));
assert.soon(() => 0 ===
                donorPrimary.adminCommand({serverStatus: 1})
                    .repl.primaryOnlyServices.TenantMigrationDonorService);

const donorRecipientMonitorPoolStats = donorPrimary.adminCommand({connPoolStats: 1}).replicaSets;
assert.eq(Object.keys(donorRecipientMonitorPoolStats).length, 0);

donorRst.stopSet();
recipientRst.stopSet();
})();
