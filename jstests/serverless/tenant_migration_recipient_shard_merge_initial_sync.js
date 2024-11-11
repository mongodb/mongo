/**
 * Tests that initial sync fails on the recipient during an active shard merge.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_71,
 *   requires_shard_merge,
 * ]
 */

import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    makeTenantDB,
    makeX509OptionsForTest,
} from "jstests/replsets/libs/tenant_migration_util.js";
import {createRstArgs} from "jstests/replsets/rslib.js";

function runInitialSyncTest(recipientMergeStage, failpoint) {
    jsTestLog("Testing initial sync with shard merge state : " + recipientMergeStage);

    const recipientRst = new ReplSetTest({
        nodes: 1,
        name: 'RecipientRst',
        serverless: true,
        nodeOptions: Object.assign(makeX509OptionsForTest().recipient)
    });
    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), sharedOptions: {nodes: 1}, recipientRst});

    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    const tenantId = ObjectId();
    const tenantDB = makeTenantDB(tenantId.str, "testDB");
    const collName = "testColl";

    jsTestLog("Add a new node to the recipient replica set");
    const initialSyncNode = recipientRst.add({
        rsConfig: {priority: 0, votes: 1},
        setParameter: {
            // Hang initial sync after fetching BeginApplyingTimestamp to ensure that the oplog
            // catchup phase of initial sync
            // receives recipient state doc oplog entries for replay.
            'failpoint.initialSyncHangBeforeCopyingDatabases': tojson({mode: 'alwaysOn'}),
            'numInitialSyncAttempts': 1,
        }
    });
    recipientRst.reInitiate();
    // Wait for the newly added node to reach initial sync state.
    recipientRst.waitForState(initialSyncNode, ReplSetTest.State.STARTUP_2);

    assert.commandWorked(initialSyncNode.adminCommand({
        waitForFailPoint: "initialSyncHangBeforeCopyingDatabases",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    const migrationUuid = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationUuid),
        recipientConnectionString: tenantMigrationTest.getRecipientConnString(),
        readPreference: {mode: 'primary'},
        tenantIds: [tenantId],
    };

    jsTestLog(`Starting the tenant migration to wait in failpoint: ${failpoint}`);
    const mergeWaitInFailPoint = configureFailPoint(recipientPrimary, failpoint, {action: "hang"});
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    let migrationThread;
    if (recipientMergeStage == TenantMigrationTest.ShardMergeRecipientState.kAborted) {
        const donorRstArgs = createRstArgs(tenantMigrationTest.getDonorRst());
        migrationThread = new Thread(async (migrationOpts, donorRstArgs) => {
            const {tryAbortMigrationAsync, forgetMigrationAsync} =
                await import("jstests/replsets/libs/tenant_migration_util.js");
            assert.commandWorked(await tryAbortMigrationAsync(migrationOpts, donorRstArgs));
            assert.commandWorked(
                await forgetMigrationAsync(migrationOpts.migrationIdString, donorRstArgs));
        }, migrationOpts, donorRstArgs);
        migrationThread.start();
    }

    // Wait for the merge to hang.
    mergeWaitInFailPoint.wait();

    const fpinitialSyncHangBeforeFinish =
        configureFailPoint(initialSyncNode, "initialSyncHangBeforeFinish", {action: "hang"});
    assert.commandWorked(initialSyncNode.adminCommand(
        {configureFailPoint: "initialSyncHangBeforeCopyingDatabases", mode: "off"}));

    fpinitialSyncHangBeforeFinish.wait();

    // Verify that the initial sync failed.
    const res = assert.commandWorked(initialSyncNode.adminCommand({replSetGetStatus: 1}));
    assert.eq(res.initialSyncStatus.failedInitialSyncAttempts, 1);
    checkLog.containsJson(initialSyncNode, 7219900);

    fpinitialSyncHangBeforeFinish.off();

    // Get rid of the failed node so the fixture can stop properly.
    recipientRst.stop(initialSyncNode);
    recipientRst.remove(initialSyncNode);
    recipientRst.reInitiate();

    // Disable the failpoint to allow merge to continue.
    mergeWaitInFailPoint.off();

    if (migrationThread !== undefined && migrationThread !== null) {
        migrationThread.join();
    }

    assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    tenantMigrationTest.stop();
    recipientRst.stopSet();
}

const stateFpMap = {
    "started": 'fpAfterPersistingTenantMigrationRecipientInstanceStateDoc',
    "learned filenames": 'pauseAfterRetrievingLastTxnMigrationRecipientInstance',
    "consistent": 'fpBeforeFetchingCommittedTransactions',
    "aborted": 'fpBeforeMarkingStateDocAsGarbageCollectable'
};

Object.entries(stateFpMap).forEach(([recipientMergeStage, failpoint]) => {
    runInitialSyncTest(recipientMergeStage, failpoint);
});
