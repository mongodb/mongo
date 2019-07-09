/**
 * This tests that writes with majority write concern will wait for at least the all committed
 * timestamp to reach the timestamp of the write. This guarantees that once a write is majority
 * committed, reading at the all committed timestamp will read that write.
 */
(function() {
    "use strict";

    load("jstests/libs/check_log.js");

    function assertWriteConcernTimeout(result) {
        assert.writeErrorWithCode(result, ErrorCodes.WriteConcernFailed);
        assert(result.hasWriteConcernError(), tojson(result));
        assert(result.getWriteConcernError().errInfo.wtimeout, tojson(result));
    }

    const rst = new ReplSetTest({name: "writes_wait_for_all_committed", nodes: 1});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const dbName = "test";
    const collName = "majority_writes_wait_for_all_committed";
    const testDB = primary.getDB(dbName);
    const testColl = testDB[collName];

    TestData.dbName = dbName;
    TestData.collName = collName;

    testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
    assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));

    assert.commandWorked(testDB.adminCommand({
        configureFailPoint: "hangAfterCollectionInserts",
        mode: "alwaysOn",
        data: {collectionNS: testColl.getFullName(), first_id: "b"}
    }));

    jsTestLog(
        "Insert a document to hang before the insert completes to hold back the all committed timestamp.");
    const joinHungWrite = startParallelShell(() => {
        assert.commandWorked(
            db.getSiblingDB(TestData.dbName)[TestData.collName].insert({_id: "b"}));
    }, primary.port);
    jsTestLog("Checking that the log contains fail point enabled.");
    checkLog.contains(
        testDB.getMongo(),
        "hangAfterCollectionInserts fail point enabled for " + testColl.getFullName());

    try {
        jsTest.log("Do a write with majority write concern that should time out.");
        assertWriteConcernTimeout(
            testColl.insert({_id: 0}, {writeConcern: {w: "majority", wtimeout: 2 * 1000}}));
    } finally {
        assert.commandWorked(
            primary.adminCommand({configureFailPoint: 'hangAfterCollectionInserts', mode: 'off'}));
    }

    joinHungWrite();
    rst.stopSet();
})();