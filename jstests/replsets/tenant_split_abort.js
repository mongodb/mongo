/*
 * Test invocation of donorAbortSplit command
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

function startReplica(numNodes) {
    const replTest = new ReplSetTest({name: 'testSet', nodes: numNodes});

    jsTestLog("Starting replica set for test");
    const donorNodes = replTest.startSet();
    replTest.initiate();

    return replTest;
}

function runSuccess() {
    "use strict";

    jsTestLog("Starting runSuccess");

    // Skip db hash check because secondary is left with a different config.
    TestData.skipCheckDBHashes = true;

    const replTest = startReplica(3);
    const primary = replTest.getPrimary();
    const adminDb = primary.getDB("admin");
    const migrationId = UUID();

    jsTestLog("Asserting no state document exist before command");
    assert.isnull(findMigration(primary, migrationId));

    jsTestLog("Running donorAbortSplit command");
    assert.commandWorked(adminDb.runCommand({donorAbortSplit: 1, migrationId: migrationId}));

    jsTestLog("Asserting state document exist after command");
    assertDocumentState(primary, migrationId, "aborted");

    replTest.stopSet();
}

runSuccess();
