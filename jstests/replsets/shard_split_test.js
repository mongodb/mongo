/*
 * Test invocation of commitShardSplit command
 * @tags: [requires_fcv_52, featureFlagShardSplit]
 */

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

const kMaxTimeMS = 1 * 1000;

function findMigration(primary, uuid) {
    const donorsCollection = primary.getDB("config").getCollection("tenantSplitDonors");
    return donorsCollection.findOne({"_id": uuid});
}

function assertDocumentState(primary, uuid, state) {
    const migrationDoc = findMigration(primary, uuid);
    assert(migrationDoc);
    assert.eq(migrationDoc.state, state);
}

function startReplica(name, numNodes) {
    const replTest = new ReplSetTest({name, nodes: numNodes});

    jsTestLog("Starting replica set for test");
    const donorNodes = replTest.startSet();
    replTest.initiate();

    return replTest;
}

function runAbort() {
    "use strict";

    jsTestLog("Starting runAbort");

    // Skip db hash check because secondary is left with a different config.
    TestData.skipCheckDBHashes = true;

    const donorSet = startReplica("donorSet", 3);
    const primary = donorSet.getPrimary();
    const adminDb = primary.getDB("admin");
    const migrationId = UUID();

    jsTestLog("Asserting no state document exist before command");
    assert.isnull(findMigration(primary, migrationId));

    jsTestLog("Running abortShardSplit command");
    assert.commandWorked(adminDb.runCommand({abortShardSplit: 1, migrationId: migrationId}));

    jsTestLog("Asserting state document exist after command");
    assertDocumentState(primary, migrationId, "aborted");

    donorSet.stopSet();
}

function runBlocking() {
    "use strict";

    jsTestLog("Starting runBlocking");

    // Skip db hash check because secondary is left with a different config.
    TestData.skipCheckDBHashes = true;

    const donorSet = startReplica("donorSet", 3);
    const recipientSet = startReplica("recipientSet", 3);
    const primary = donorSet.getPrimary();
    const adminDb = primary.getDB("admin");
    const migrationId = UUID();

    const tenantId1 = "test_tenant_1";
    const tenantId2 = "test_tenant_2";
    const tenants = [tenantId1, tenantId2];

    jsTestLog("Asserting no state document exist before command");
    assert.isnull(findMigration(primary, migrationId));

    jsTestLog("Asserting we can write before the migration");
    tenants.forEach(id => {
        const tenantDB = primary.getDB(id + "_data");
        let insertedObj = {name: id + "1", payload: "testing_data"};
        assert.commandWorked(tenantDB.runCommand(
            {insert: "testing_collection", documents: [insertedObj], maxTimeMS: kMaxTimeMS}));
    });

    jsTestLog("Inserting failpoint after blocking");
    let blockingFailPoint = configureFailPoint(adminDb, "pauseShardSplitAfterBlocking");

    jsTestLog("Running commitShardSplit command");
    const awaitCommand = startParallelShell(
        funWithArgs(function(migrationId, url, tenants) {
            assert.commandWorked(db.adminCommand({
                commitShardSplit: 1,
                migrationId: migrationId,
                recipientConnectionString: url,
                "tenantIds": tenants
            }));
        }, migrationId, recipientSet.getURL(), [tenantId1, tenantId2]), donorSet.getPrimary().port);

    blockingFailPoint.wait();

    jsTestLog("Asserting state document is in blocking state");
    assertDocumentState(primary, migrationId, "blocking");

    jsTestLog("Asserting we cannot write in blocking state");
    tenants.forEach(id => {
        const tenantDB = primary.getDB(id + "_data");
        let insertedObj = {name: id + "2", payload: "testing_data2"};
        let res = tenantDB.runCommand(
            {insert: "testing_collection", documents: [insertedObj], maxTimeMS: kMaxTimeMS});
        assert.commandFailedWithCode(res, ErrorCodes.MaxTimeMSExpired);
    });

    blockingFailPoint.off();
    awaitCommand();

    jsTestLog("Asserting state document exist after command");
    assertDocumentState(primary, migrationId, "committed");

    // If we validate, it will try to list all collections and the migrated collections will return
    // a TenantMigrationCommitted error.
    donorSet.stopSet(undefined /* signal */, false /* forRestart */, {skipValidation: 1});
    recipientSet.stopSet();
}

runAbort();
runBlocking();
