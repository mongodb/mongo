/**
 * Tests startup recovery to tenant migration donor's in memory state.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, requires_persistence,
 * incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/parallelTester.js");

// An object that mirrors the access states for the TenantMigrationAccessBlocker.
const accessState = {
    kAllow: 0,
    kBlockingWrites: 1,
    kBlockingReadsAndWrites: 2,
    kReject: 3
};

const donorRst = new ReplSetTest({
    nodes: 1,
    name: 'donor',
    nodeOptions: {
        setParameter: {
            enableTenantMigrations: true,
            "failpoint.PrimaryOnlyServiceSkipRebuildingInstances": tojson({mode: "alwaysOn"})
        }
    }
});
const recipientRst = new ReplSetTest(
    {nodes: 1, name: 'recipient', nodeOptions: {setParameter: {enableTenantMigrations: true}}});

donorRst.startSet();
donorRst.initiate();

recipientRst.startSet();
recipientRst.initiate();

const kMaxSleepTimeMS = 1000;
const kDBPrefix = 'testDb';
const kConfigDonorsNS = "config.tenantMigrationDonors";

let donorPrimary = donorRst.getPrimary();
let kRecipientConnString = recipientRst.getURL();
let configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);

function startMigration(host, recipientConnString, dbPrefix) {
    const primary = new Mongo(host);
    try {
        primary.adminCommand({
            donorStartMigration: 1,
            migrationId: UUID(),
            recipientConnectionString: recipientConnString,
            databasePrefix: dbPrefix,
            readPreference: {mode: "primary"}
        });
    } catch (e) {
        if (isNetworkError(e)) {
            jsTestLog('Ignoring network error due to node being restarted: ' + tojson(e));
            return;
        }
        throw e;
    }
}

let migrationThread =
    new Thread(startMigration, donorPrimary.host, kRecipientConnString, kDBPrefix);
migrationThread.start();
sleep(Math.random() * kMaxSleepTimeMS);
donorRst.stopSet(null /* signal */, true /*forRestart */);
donorRst.startSet({
    restart: true,
    setParameter: {
        enableTenantMigrations: true,
        "failpoint.PrimaryOnlyServiceSkipRebuildingInstances": "{'mode':'alwaysOn'}"
    }
});
donorPrimary = donorRst.getPrimary();

configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);
let donorDoc = configDonorsColl.findOne({databasePrefix: kDBPrefix});
let mtab = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
if (donorDoc) {
    let state = donorDoc.state;
    switch (state) {
        case "data sync":
            assert.soon(() => mtab[kDBPrefix].access == accessState.kAllow);
            break;
        case "blocking":
            assert.soon(() => mtab[kDBPrefix].access == accessState.kBlockingReadsAndWrites);
            assert.soon(
                () => bsonWoCompare(mtab[kDBPrefix].blockTimestamp, donorDoc.blockTimestamp) == 0);
            break;
        case "committed":
            assert.soon(() => mtab[kDBPrefix].access == accessState.kReject);
            assert.soon(() => bsonWoCompare(mtab[kDBPrefix].commitOrAbortOpTime,
                                            donorDoc.commitOrAbortOpTime) == 0);
            assert.soon(
                () => bsonWoCompare(mtab[kDBPrefix].blockTimestamp, donorDoc.blockTimestamp) == 0);
            break;
        case "aborted":
            assert.soon(() => mtab[kDBPrefix].access == accessState.kAllow);
            assert.soon(() => bsonWoCompare(mtab[kDBPrefix].commitOrAbortOpTime,
                                            donorDoc.commitOrAbortOpTime) == 0);
            assert.soon(
                () => bsonWoCompare(mtab[kDBPrefix].blockTimestamp, donorDoc.blockTimestamp) == 0);
            break;
        default:
            throw new Error(`Invalid state "${state}" from donor doc.`);
    }
}
migrationThread.join();
donorRst.stopSet();
recipientRst.stopSet();
})();
