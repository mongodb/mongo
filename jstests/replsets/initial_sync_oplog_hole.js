/**
 * Test that initial sync works without error when the sync source has an oplog hole.
 *
 * @tags: [requires_document_locking]
 */
(function() {
    "use strict";

    load("jstests/libs/check_log.js");
    load("jstests/replsets/rslib.js");

    // Set up replica set. Disallow chaining so nodes always sync from primary.
    const testName = "initial_sync_oplog_hole";
    const dbName = testName;
    // Set up a three-node replset.  The first node is primary throughout the test, the second node
    // is used as the initial sync node, and the third node is to ensure we maintain a majority (and
    // thus no election) while restarting the second.
    const replTest = new ReplSetTest({
        name: testName,
        nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
        settings: {chainingAllowed: false}
    });
    replTest.startSet();
    replTest.initiate();

    const primary = replTest.getPrimary();
    const primaryDB = primary.getDB(dbName);
    const secondary = replTest.getSecondary();
    const secondaryDB = secondary.getDB(dbName);
    const collName = "testcoll";
    const primaryColl = primaryDB[collName];
    const secondaryColl = secondaryDB[collName];
    const nss = primaryColl.getFullName();
    TestData.testName = testName;
    TestData.collectionName = collName;

    jsTestLog("Writing data before oplog hole to collection.");
    assert.writeOK(primaryColl.insert({_id: "a"}));
    // Make sure it gets written out.
    assert.eq(primaryColl.find({_id: "a"}).itcount(), 1);

    jsTest.log("Create the uncommitted write.");
    assert.commandWorked(primaryDB.adminCommand({
        configureFailPoint: "hangAfterCollectionInserts",
        mode: "alwaysOn",
        data: {collectionNS: primaryColl.getFullName(), first_id: "b"}
    }));

    const db = primaryDB;
    const joinHungWrite = startParallelShell(() => {
        assert.commandWorked(
            db.getSiblingDB(TestData.testName)[TestData.collectionName].insert({_id: "b"}));
    }, primary.port);
    checkLog.contains(
        primaryDB.getMongo(),
        "hangAfterCollectionInserts fail point enabled for " + primaryColl.getFullName());

    jsTest.log("Create a write following the uncommitted write.");
    assert.writeOK(primaryColl.insert({_id: "c"}));
    // Make sure it gets written out.
    assert.eq(primaryColl.find({_id: "c"}).itcount(), 1);

    jsTestLog("Restarting initial sync node.");
    replTest.restart(secondary, {startClean: true});
    jsTestLog("Waiting for initial sync to start.");
    checkLog.contains(secondaryDB.getMongo(), "Starting initial sync");

    // The 5 seconds is because in the non-buggy case, we'll be hung waiting for the optime to
    // advance.  However, if we allow the write to finish immediately, we are likely to miss the
    // race if it happens.  By allowing 5 seconds, we'll never fail when we should succeed, and
    // we'll nearly always fail when we should fail.
    //
    // Once the hangAfterCollectionInserts failpoint is turned off, the write of {_id: "b"} will
    // complete and both the data and the oplog entry for the write will be written out. The oplog
    // visibility thread will then close the oplog hole.
    jsTestLog("Allow the uncommitted write to finish in 5 seconds.");
    const joinDisableFailPoint = startParallelShell(() => {
        sleep(5000);
        assert.commandWorked(
            db.adminCommand({configureFailPoint: "hangAfterCollectionInserts", mode: "off"}));
    }, primary.port);

    jsTestLog("Waiting for initial sync to complete.");
    waitForState(secondary, ReplSetTest.State.SECONDARY);

    jsTestLog("Joining hung write");
    joinDisableFailPoint();
    joinHungWrite();

    jsTestLog("Checking that primary has all data items.");
    // Make sure the primary collection has all three data items.
    assert.docEq(primaryColl.find().toArray(), [{"_id": "a"}, {"_id": "b"}, {"_id": "c"}]);

    jsTestLog("Checking that secondary has all data items.");
    replTest.awaitReplication();
    assert.docEq(secondaryColl.find().toArray(), [{"_id": "a"}, {"_id": "b"}, {"_id": "c"}]);

    replTest.stopSet();
})();
