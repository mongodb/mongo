/*
 * Test invocation of commitShardSplit command
 * @tags: [requires_fcv_52, featureFlagShardSplit]
 */

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');
load("jstests/serverless/libs/basic_serverless_test.js");

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

function runAbort() {
    "use strict";

    jsTestLog("Starting runAbort");

    // Skip db hash check because secondary is left with a different config.
    TestData.skipCheckDBHashes = true;

    const test =
        new BasicServerlessTest({recipientTagName: "recipientNode", recipientSetName: "recipient"});
    test.addRecipientNodes();

    const donorPrimary = test.donor.getPrimary();
    const adminDb = donorPrimary.getDB("admin");
    const migrationId = UUID();

    jsTestLog("Asserting no state document exist before command");
    assert.isnull(findMigration(donorPrimary, migrationId));

    jsTestLog("Running abortShardSplit command");
    assert.commandWorked(adminDb.runCommand({abortShardSplit: 1, migrationId}));

    jsTestLog("Asserting state document exist after command");
    assertDocumentState(donorPrimary, migrationId, "aborted");

    test.stop();
}

function runBlocking() {
    "use strict";

    jsTestLog("Starting runBlocking");

    // Skip db hash check because secondary is left with a different config.
    TestData.skipCheckDBHashes = true;

    const test =
        new BasicServerlessTest({recipientTagName: "recipientNode", recipientSetName: "recipient"});
    test.addRecipientNodes();

    const donorPrimary = test.donor.getPrimary();
    const migrationId = UUID();
    const tenantIds = ["test_tenant_1", "test_tenant_2"];

    jsTestLog("Asserting no state document exist before command");
    assert.isnull(findMigration(donorPrimary, migrationId));

    jsTestLog("Asserting we can write before the migration");
    tenantIds.forEach(id => {
        const tenantDB = donorPrimary.getDB(id + "_data");
        let insertedObj = {name: id + "1", payload: "testing_data"};
        assert.commandWorked(tenantDB.runCommand(
            {insert: "testing_collection", documents: [insertedObj], maxTimeMS: kMaxTimeMS}));
    });

    // configure failpoints
    const adminDb = donorPrimary.getDB("admin");
    const blockingFailPoint = configureFailPoint(adminDb, "pauseShardSplitAfterBlocking");

    // TODO(SERVER-63091): remove this when we actually split recipients
    configureFailPoint(adminDb, "skipShardSplitWaitForSplitAcceptance");

    jsTestLog("Running commitShardSplit command");
    const awaitCommand = startParallelShell(
        funWithArgs(
            function(migrationId, recipientTagName, recipientSetName, tenantIds) {
                assert.commandWorked(db.adminCommand({
                    commitShardSplit: 1,
                    migrationId,
                    recipientTagName,
                    recipientSetName,
                    tenantIds
                }));
            },
            migrationId,
            test.recipientTagName,
            test.recipientSetName,
            tenantIds),
        donorPrimary.port);

    blockingFailPoint.wait();

    jsTestLog("Asserting state document is in blocking state");
    assertDocumentState(donorPrimary, migrationId, "blocking");

    jsTestLog("Asserting we cannot write in blocking state");
    tenantIds.forEach(id => {
        const tenantDB = donorPrimary.getDB(id + "_data");
        let insertedObj = {name: id + "2", payload: "testing_data2"};
        let res = tenantDB.runCommand(
            {insert: "testing_collection", documents: [insertedObj], maxTimeMS: kMaxTimeMS});
        assert.commandFailedWithCode(res, ErrorCodes.MaxTimeMSExpired);
    });

    jsTestLog("Disabling failpoints and waiting for command to complete");
    blockingFailPoint.off();
    awaitCommand();

    jsTestLog("Asserting state document exist after command");
    assertDocumentState(donorPrimary, migrationId, "committed");
    test.stop();
}

runAbort();
runBlocking();
