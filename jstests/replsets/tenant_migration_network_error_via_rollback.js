/**
 * This test does sync source as a way to test that tenant migrations are resilient to transient
 * connection errors between the recipient primary and the sync source at various stages in the
 * process. (Replica set members close connections as part of rollback.)
 *
 * TODO SERVER-61231: shard merge can't handle concurrent rollback, adapt this test.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");  // for 'extractUUIDFromObject'
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/replsets/rslib.js");  // for 'stopServerReplication'

function runTest({failPointName, failPointData = {}, batchSize = 10 * 1000}) {
    const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();

    const donorRst = new ReplSetTest({
        name: "recipientRst",
        nodes: [
            {rsConfig: {priority: 1}},  // initial primary
            {},                         // one possible next primary
            {},                         // the other next primary
            {rsConfig: {priority: 0, hidden: true}},
            {rsConfig: {priority: 0, hidden: true}}
        ],
        nodeOptions: Object.assign(migrationX509Options.donor, {
            setParameter: {
                // Allow non-timestamped reads on donor after migration completes for testing.
                'failpoint.tenantMigrationDonorAllowsNonTimestampedReads':
                    tojson({mode: 'alwaysOn'}),
            }
        }),
        settings: {catchUpTimeoutMillis: 0, chainingAllowed: false}
    });
    const donorNodes = donorRst.startSet();
    donorRst.initiate();

    // We make D and E hidden to reduce the number of readable secondaries. Thus, when the data sync
    // begins (with A as primary), we can be sure that the sync source is either B or C.
    const [donorA, donorB, donorC, donorD, donorE] = donorNodes;

    jsTestLog("Setting up node A as initial donor primary");
    assert.eq(donorA, donorRst.getPrimary());
    donorRst.awaitReplication();

    const tenantMigrationTest = new TenantMigrationTest({
        name: jsTestName(),
        donorRst: donorRst,
        sharedOptions: {setParameter: {collectionClonerBatchSize: batchSize}}
    });

    const tenantId = "testTenantId";
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const collName = "testColl";

    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    tenantMigrationTest.insertDonorDB(dbName, collName);

    const migrationId = UUID();
    const migrationIdString = extractUUIDFromObject(migrationId);
    const migrationOpts = {
        migrationIdString: migrationIdString,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId: tenantId,
        readPreference: {mode: 'secondary'},
    };

    // Kick off a tenant migration and make sure that the recipient has started syncing.
    const fullData = Object.merge(failPointData, {database: dbName});
    const hangWhileMigrating = configureFailPoint(recipientPrimary, failPointName, fullData);

    jsTestLog("Starting migration and waiting for recipient to hit the failpoint");
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    hangWhileMigrating.wait();

    jsTestLog("Determining whether B or C is the sync source");
    let res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
    assert.eq(res.inprog.length, 1, () => tojson(res));
    let currOp = res.inprog[0];
    assert.eq(bsonWoCompare(currOp.instanceID, migrationId), 0, () => tojson(res));

    // At this point the sync source should be either B or C.
    let syncSource;
    let nextPrimary;

    if (donorB.host == currOp.donorSyncSource) {
        syncSource = donorB;
        nextPrimary = donorC;
    } else {
        syncSource = donorC;
        nextPrimary = donorB;
    }

    jsTestLog("Sync source: " + syncSource);
    jsTestLog("Next primary: " + nextPrimary);

    stopServerReplication(nextPrimary);
    stopServerReplication(donorD);
    stopServerReplication(donorE);

    jsTestLog("Inserting rollback operations");
    assert.commandWorked(donorA.getDB("rollbackDB").rollbackColl.insert({toBe: 'rolledBack'}, {
        writeConcern: {w: 2}
    }));

    const lastRBID = assert.commandWorked(syncSource.adminCommand("replSetGetRBID")).rbid;
    const rollbackFP = configureFailPoint(syncSource, "rollbackHangBeforeStart");

    jsTestLog("Failing over to next primary");
    assert.commandWorked(
        donorA.adminCommand({replSetStepDown: ReplSetTest.kDefaultTimeoutMS, force: true}));
    donorRst.stepUp(nextPrimary, {awaitReplicationBeforeStepUp: false});
    assert.eq(nextPrimary, donorRst.getPrimary());
    restartServerReplication(nextPrimary);
    restartServerReplication(donorD);
    restartServerReplication(donorE);

    res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
    currOp = res.inprog[0];
    assert.eq(currOp.migrationCompleted, false, () => tojson(currOp));
    assert.eq(currOp.dataSyncCompleted, false, () => tojson(currOp));

    // The sync source has not rolled back, so the recipient has not yet perceived an interruption.
    assert.eq(NumberLong(0), currOp.numRestartsDueToDonorConnectionFailure, () => tojson(currOp));

    jsTestLog("Inserting new documents and waiting for sync source to roll back");
    assert.commandWorked(
        nextPrimary.getDB("otherDB").otherColl.insert({toBe: 'kept'}, {writeConcern: {w: 3}}));

    rollbackFP.wait();
    rollbackFP.off();

    assert.soonNoExcept(function() {
        const rbid = assert.commandWorked(syncSource.adminCommand("replSetGetRBID")).rbid;
        return rbid > lastRBID;
    }, "rbid did not update", ReplSetTest.kDefaultTimeoutMS);

    hangWhileMigrating.off();

    jsTestLog("Waiting for migration to finish");
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    tenantMigrationTest.waitForDonorNodesToReachState(
        donorRst.nodes, migrationId, tenantId, TenantMigrationTest.DonorState.kCommitted);

    res = recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"});
    currOp = res.inprog[0];

    // The migration should still be considered active as it is not yet forgotten.
    assert.eq(currOp.migrationCompleted, false, () => tojson(currOp));
    assert.eq(currOp.dataSyncCompleted, false, () => tojson(currOp));

    // A restart was necessary, due to the sync source closing connections on rollback.
    assert.eq(NumberLong(1), currOp.numRestartsDueToDonorConnectionFailure, () => tojson(currOp));

    jsTestLog("Forgetting migration");
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    donorRst.stopSet();
    tenantMigrationTest.stop();
}

// Only pick 1 test per run to save machine time.
const caseNum = Math.floor(Math.random() * 11) + 1;

switch (caseNum) {
    case 1:
        jsTestLog("[1] Testing rollback before fetching clusterTime keys.");
        runTest({
            failPointName: "fpBeforeFetchingDonorClusterTimeKeys",
            failPointData: {
                action: "hang",
            },
        });
        break;

    case 2:
        jsTestLog("[2] Testing rollback after connecting instances.");
        runTest({
            failPointName: "fpAfterConnectingTenantMigrationRecipientInstance",
            failPointData: {
                action: "hang",
            },
        });
        break;

    case 3:
        jsTestLog("[3] Testing rollback after comparing FCV against donor.");
        runTest({
            failPointName: "fpAfterComparingRecipientAndDonorFCV",
            failPointData: {
                action: "hang",
            },
        });
        break;

    case 4:
        jsTestLog("[4] Testing rollback after fetching start opTime.");
        runTest({
            failPointName: "fpAfterRetrievingStartOpTimesMigrationRecipientInstance",
            failPointData: {
                action: "hang",
            },
        });
        break;

    case 5:
        jsTestLog("[5] Testing rollback before starting oplog fetcher.");
        runTest({
            failPointName: "fpAfterFetchingRetryableWritesEntriesBeforeStartOpTime",
            failPointData: {
                action: "hang",
            },
        });
        break;

    case 6:
        jsTestLog("[6] Testing rollback during cloning, after starting data sync.");
        runTest({
            failPointName: "hangBeforeClonerStage",
            failPointData: {
                cloner: "TenantAllDatabaseCloner",
                stage: "listDatabases",
            }
        });
        break;

    case 7:
        jsTestLog("[7] Testing rollback during cloning, before cloning databases.");
        runTest({
            failPointName: "hangBeforeClonerStage",
            failPointData: {
                cloner: "TenantDatabaseCloner",
                stage: "listCollections",
            }
        });
        break;

    case 8:
        jsTestLog("[8] Testing rollback during cloning, before listing indexes.");
        runTest({
            failPointName: "hangBeforeClonerStage",
            failPointData: {
                cloner: "TenantCollectionCloner",
                stage: "listIndexes",
            }
        });
        break;

    case 9:
        jsTestLog("[9] Testing rollback during cloning, between listIndexes and find.");
        runTest({
            failPointName: "hangBeforeClonerStage",
            failPointData: {
                cloner: "TenantCollectionCloner",
                stage: "query",
            }
        });
        break;

    case 10:
        jsTestLog("[10] Testing rollback during cloning, between getMores.");
        runTest({
            failPointName: "tenantMigrationHangCollectionClonerAfterHandlingBatchResponse",
            batchSize: 1,
        });
        break;

    case 11:
        jsTestLog("[11] Testing rollback after finishing cloning.");
        runTest({
            failPointName: "fpBeforeFetchingCommittedTransactions",
            failPointData: {
                action: "hang",
            },
        });
        break;
    default:
        // Unreachable.
        assert(false);
}
})();
