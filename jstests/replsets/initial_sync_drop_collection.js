// Test that CollectionCloner completes without error when a collection is dropped during cloning.

(function() {
    "use strict";

    // Skip db hash check because secondary cannot complete initial sync.
    TestData.skipCheckDBHashes = true;

    load("jstests/libs/check_log.js");
    load('jstests/replsets/libs/two_phase_drops.js');
    load("jstests/libs/uuid_util.js");

    // Set up replica set. Disallow chaining so nodes always sync from primary.
    const testName = "initial_sync_drop_collection";
    const dbName = testName;
    var replTest = new ReplSetTest({
        name: testName,
        nodes: [{}, {rsConfig: {priority: 0}}],
        settings: {chainingAllowed: false}
    });
    replTest.startSet();
    replTest.initiate();

    var primary = replTest.getPrimary();
    var primaryDB = primary.getDB(dbName);
    var secondary = replTest.getSecondary();
    var secondaryDB = secondary.getDB(dbName);
    const collName = "testcoll";
    var primaryColl = primaryDB[collName];
    var secondaryColl = secondaryDB[collName];
    var pRenameColl = primaryDB["r_" + collName];
    var nss = primaryColl.getFullName();

    // This function adds data to the collection, restarts the secondary node with the given
    // parameters and setting the given failpoint, waits for the failpoint to be hit,
    // drops the collection, then disables the failpoint.  It then optionally waits for the
    // expectedLog message and waits for the secondary to complete initial sync, then ensures
    // the collection on the secondary is empty.
    function setupTest({failPoint, secondaryStartupParams}) {
        jsTestLog("Writing data to collection.");
        assert.writeOK(primaryColl.insert([{_id: 1}, {_id: 2}]));

        jsTestLog("Restarting secondary with failPoint " + failPoint + " set for " + nss);
        secondaryStartupParams = secondaryStartupParams || {};
        secondaryStartupParams['failpoint.' + failPoint] =
            tojson({mode: 'alwaysOn', data: {nss: nss}});
        replTest.restart(secondary, {startClean: true, setParameter: secondaryStartupParams});

        jsTestLog("Waiting for secondary to reach failPoint " + failPoint);
        checkLog.contains(secondary, failPoint + " fail point enabled for " + nss);

        // Restarting the secondary may have resulted in an election.  Wait until the system
        // stabilizes and reaches RS_STARTUP2 state.
        replTest.getPrimary();
        replTest.waitForState(secondary, ReplSetTest.State.STARTUP_2);
    }

    function finishTest({failPoint, secondaryStartupParams, expectedLog, waitForDrop, createNew}) {
        // Get the uuid for use in checking the log line.
        let uuid = getUUIDFromListCollections(primaryDB, collName);

        jsTestLog("Dropping collection on primary: " + primaryColl.getFullName());
        assert(primaryColl.drop());

        if (waitForDrop) {
            jsTestLog("Waiting for drop to commit on primary");
            TwoPhaseDropCollectionTest.waitForDropToComplete(primaryDB, collName);
        }

        if (createNew) {
            jsTestLog("Creating a new collection with the same name: " + primaryColl.getFullName());
            assert.writeOK(primaryColl.insert({_id: "not the same collection"}));
        }

        jsTestLog("Allowing secondary to continue.");
        assert.commandWorked(secondary.adminCommand({configureFailPoint: failPoint, mode: 'off'}));

        if (expectedLog) {
            jsTestLog(eval(expectedLog));
            checkLog.contains(secondary, eval(expectedLog));
        }

        jsTestLog("Waiting for initial sync to complete.");
        replTest.waitForState(secondary, ReplSetTest.State.SECONDARY);

        let res =
            assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1, initialSync: 1}));
        assert.eq(0, res.initialSyncStatus.failedInitialSyncAttempts);

        if (createNew) {
            assert.eq([{_id: "not the same collection"}], secondaryColl.find().toArray());
            assert(primaryColl.drop());
        } else {
            assert.eq(0, secondaryColl.find().itcount());
        }
        replTest.checkReplicatedDataHashes();
    }

    function runDropTest(params) {
        setupTest(params);
        finishTest(params);
    }

    jsTestLog("Testing dropping between listIndexes and find.");
    runDropTest({failPoint: "initialSyncHangCollectionClonerBeforeEstablishingCursor"});

    jsTestLog(
        "Testing dropping between listIndexes and find, with new same-name collection created.");
    runDropTest(
        {failPoint: "initialSyncHangCollectionClonerBeforeEstablishingCursor", createNew: true});

    jsTestLog("Testing drop-pending between getMore calls.");
    runDropTest({
        failPoint: "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
        secondaryStartupParams: {collectionClonerBatchSize: 1},
        expectedLog:
            "`CollectionCloner ns: '${nss}' uuid: ${uuid} stopped because collection was dropped.`"
    });

    jsTestLog("Testing drop-pending with new same-name collection created, between getMore calls.");
    runDropTest({
        failPoint: "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
        secondaryStartupParams: {collectionClonerBatchSize: 1},
        expectedLog:
            "`CollectionCloner ns: '${nss}' uuid: ${uuid} stopped because collection was dropped.`",
        createNew: true
    });

    jsTestLog("Testing committed drop between getMore calls.");

    // Add another node to the set, so when we drop the collection it can commit.  This other
    // secondary will be finished with initial sync when the drop happens.
    var secondary2 = replTest.add({rsConfig: {priority: 0}});
    replTest.reInitiate();
    replTest.waitForState(secondary2, ReplSetTest.State.SECONDARY);

    runDropTest({
        failPoint: "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
        secondaryStartupParams: {collectionClonerBatchSize: 1},
        waitForDrop: true,
        expectedLog:
            "`CollectionCloner ns: '${nss}' uuid: ${uuid} stopped because collection was dropped.`"
    });

    jsTestLog("Testing rename between getMores.");
    setupTest({
        failPoint: "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
        secondaryStartupParams: {collectionClonerBatchSize: 1},
    });
    jsTestLog("Renaming collection on primary");
    assert.commandWorked(primary.adminCommand({
        renameCollection: primaryColl.getFullName(),
        to: pRenameColl.getFullName(),
        dropTarget: false
    }));

    jsTestLog("Allowing secondary to continue.");
    // Make sure we don't reach the fassert() indicating initial sync failure.
    assert.commandWorked(secondary.adminCommand(
        {configureFailPoint: "initialSyncHangBeforeFinish", mode: 'alwaysOn'}));

    assert.commandWorked(secondary.adminCommand({
        configureFailPoint: "initialSyncHangCollectionClonerAfterHandlingBatchResponse",
        mode: 'off'
    }));
    jsTestLog("Waiting for initial sync to complete.");
    checkLog.contains(secondary,
                      "The maximum number of retries have been exhausted for initial sync.");
    replTest.stopSet();
})();
