/*
 * Test invocation of commitShardSplit command
 * @tags: [requires_fcv_52, featureFlagShardSplit]
 */

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

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

function runStart() {
    "use strict";

    jsTestLog("Starting runStart");

    // Skip db hash check because secondary is left with a different config.
    TestData.skipCheckDBHashes = true;

    const donorSet = startReplica("donorSet", 3);
    const recipientSet = startReplica("recipientSet", 3);
    const primary = donorSet.getPrimary();
    const adminDb = primary.getDB("admin");
    const migrationId = UUID();

    const tenantId1 = "test_tenant_1";
    const tenantId2 = "test_tenant_2";

    jsTestLog("Asserting no state document exist before command");
    assert.isnull(findMigration(primary, migrationId));

    jsTestLog("Running commitShardSplit command");
    assert.commandWorked(adminDb.runCommand({
        commitShardSplit: 1,
        migrationId: migrationId,
        recipientConnectionString: recipientSet.getURL(),
        "tenantIds": [tenantId1, tenantId2]
    }));

    jsTestLog("Asserting state document exist after command");
    assertDocumentState(primary, migrationId, "data sync");

    donorSet.stopSet();
    recipientSet.stopSet();
}

runAbort();
runStart();
