/**
 * Test that oplog (on both primary and secondary) rolls over when its size exceeds the configured
 * maximum. This test runs on wiredTiger storage engine and inMemory storage engine (if available).
 */
(function() {
"use strict";

function doTest(storageEngine) {
    jsTestLog("Testing with storageEngine: " + storageEngine);

    // Pause the oplog cap maintainer thread for this test until oplog truncation is needed. The
    // truncation thread can hold a mutex for a short period of time which prevents new oplog stones
    // from being created during an insertion if the mutex cannot be obtained immediately. Instead,
    // the next insertion will attempt to create a new oplog stone, which this test does not do.
    const replSet = new ReplSetTest({
        // Set the syncdelay to 1s to speed up checkpointing.
        nodeOptions: {
            syncdelay: 1,
            setParameter: {
                logComponentVerbosity: tojson({storage: 2}),
                'failpoint.hangOplogCapMaintainerThread': tojson({mode: 'alwaysOn'})
            }
        },
        nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]
    });
    // Set max oplog size to 1MB.
    replSet.startSet({storageEngine: storageEngine, oplogSize: 1});
    replSet.initiate();

    const primary = replSet.getPrimary();
    const primaryOplog = primary.getDB("local").oplog.rs;
    const secondary = replSet.getSecondary();
    const secondaryOplog = secondary.getDB("local").oplog.rs;

    // Verify that the oplog cap maintainer thread is paused.
    checkLog.contains(primary, "Hanging the oplog cap maintainer thread due to fail point");
    checkLog.contains(secondary, "Hanging the oplog cap maintainer thread due to fail point");

    const coll = primary.getDB("test").foo;
    // 400KB each so that oplog can keep at most two insert oplog entries.
    const longString = new Array(400 * 1024).join("a");

    function numInsertOplogEntry(oplog) {
        print(`Oplog times for ${oplog.getMongo().host}: ${
            tojsononeline(oplog.find().projection({ts: 1, t: 1, op: 1, ns: 1}).toArray())}`);
        return oplog.find({op: "i", "ns": "test.foo"}).itcount();
    }

    // Insert the first document.
    const firstInsertTimestamp =
        assert
            .commandWorked(coll.runCommand(
                "insert", {documents: [{_id: 0, longString: longString}], writeConcern: {w: 2}}))
            .operationTime;
    jsTestLog("First insert timestamp: " + firstInsertTimestamp);

    // Test that oplog entry of the first insert exists on both primary and secondary.
    assert.eq(1, numInsertOplogEntry(primaryOplog));
    assert.eq(1, numInsertOplogEntry(secondaryOplog));

    // Insert the second document.
    const secondInsertTimestamp =
        assert
            .commandWorked(coll.runCommand(
                "insert", {documents: [{_id: 1, longString: longString}], writeConcern: {w: 2}}))
            .operationTime;
    jsTestLog("Second insert timestamp: " + secondInsertTimestamp);

    // Test that oplog entries of both inserts exist on both primary and secondary.
    assert.eq(2, numInsertOplogEntry(primaryOplog));
    assert.eq(2, numInsertOplogEntry(secondaryOplog));

    // Have a more fine-grained test for enableMajorityReadConcern=true to also test oplog
    // truncation happens at the time we expect it to happen. When
    // enableMajorityReadConcern=false the lastStableRecoveryTimestamp is not available, so
    // switch to a coarser-grained mode to only test that oplog truncation will eventually
    // happen when oplog size exceeds the configured maximum.
    if (primary.getDB('admin').serverStatus().storageEngine.supportsCommittedReads) {
        const awaitCheckpointer = function(timestamp) {
            assert.soon(
                () => {
                    const primaryTimestamp =
                        assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}))
                            .lastStableRecoveryTimestamp;
                    const secondaryTimestamp =
                        assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}))
                            .lastStableRecoveryTimestamp;
                    jsTestLog("Awaiting last stable recovery timestamp " +
                              `(primary: ${tojson(primaryTimestamp)}, secondary: ${
                                  tojson(secondaryTimestamp)}) ` +
                              `target: ${tojson(timestamp)}`);
                    return ((timestampCmp(primaryTimestamp, timestamp) >= 0) &&
                            (timestampCmp(secondaryTimestamp, timestamp) >= 0));
                },
                "Timeout waiting for checkpointing to catch up",
                ReplSetTest.kDefaultTimeoutMS,
                2000);
        };

        // Wait for checkpointing/stable timestamp to catch up with the second insert so oplog
        // entry of the first insert is allowed to be deleted by the oplog cap maintainer thread
        // when a new oplog stone is created. "inMemory" WT engine does not run checkpoint
        // thread and lastStableRecoveryTimestamp is the stable timestamp in this case.
        awaitCheckpointer(secondInsertTimestamp);

        // Insert the third document which will trigger a new oplog stone to be created. The
        // oplog truncater thread will then be unblocked on the creation of the new oplog stone
        // and will start truncating oplog entries. The oplog entry for the first insert will be
        // truncated after the oplog truncater thread finishes.
        const thirdInsertTimestamp =
            assert
                .commandWorked(coll.runCommand(
                    "insert",
                    {documents: [{_id: 2, longString: longString}], writeConcern: {w: 2}}))
                .operationTime;
        jsTestLog("Third insert timestamp: " + thirdInsertTimestamp);

        // There is a race between how we calculate the pinnedOplog and checkpointing. The timestamp
        // of the pinnedOplog could be less than the actual stable timestamp used in a checkpoint.
        // Wait for the checkpointer to run for another round to make sure the first insert oplog is
        // not pinned.
        awaitCheckpointer(thirdInsertTimestamp);

        // Verify that there are three oplog entries while the oplog cap maintainer thread is
        // paused.
        assert.eq(3, numInsertOplogEntry(primaryOplog));
        assert.eq(3, numInsertOplogEntry(secondaryOplog));

        // Let the oplog cap maintainer thread start truncating the oplog.
        assert.commandWorked(primary.adminCommand(
            {configureFailPoint: "hangOplogCapMaintainerThread", mode: "off"}));
        assert.commandWorked(secondary.adminCommand(
            {configureFailPoint: "hangOplogCapMaintainerThread", mode: "off"}));

        // Test that oplog entry of the initial insert rolls over on both primary and secondary.
        // Use assert.soon to wait for oplog truncater thread to run.
        assert.soon(() => {
            return numInsertOplogEntry(primaryOplog) === 2;
        }, "Timeout waiting for oplog to roll over on primary");
        assert.soon(() => {
            return numInsertOplogEntry(secondaryOplog) === 2;
        }, "Timeout waiting for oplog to roll over on secondary");

        if (jsTest.options().storageEngine == "wiredTiger") {
            const res = primary.getDB("test").runCommand({serverStatus: 1});
            assert.commandWorked(res);
            assert.eq(res.oplogTruncation.truncateCount, 1, tojson(res.oplogTruncation));
            assert.gt(
                res.oplogTruncation.totalTimeTruncatingMicros, 0, tojson(res.oplogTruncation));
        }
    } else {
        // Let the oplog cap maintainer thread start truncating the oplog.
        assert.commandWorked(primary.adminCommand(
            {configureFailPoint: "hangOplogCapMaintainerThread", mode: "off"}));
        assert.commandWorked(secondary.adminCommand(
            {configureFailPoint: "hangOplogCapMaintainerThread", mode: "off"}));

        // Only test that oplog truncation will eventually happen.
        let numInserted = 2;
        assert.soon(function() {
            // Insert more documents.
            assert.commandWorked(
                coll.insert({_id: numInserted++, longString: longString}, {writeConcern: {w: 2}}));
            const numInsertOplogEntryPrimary = numInsertOplogEntry(primaryOplog);
            const numInsertOplogEntrySecondary = numInsertOplogEntry(secondaryOplog);
            // Oplog has been truncated if the number of insert oplog entries is less than
            // number of inserted.
            if (numInsertOplogEntryPrimary < numInserted &&
                numInsertOplogEntrySecondary < numInserted)
                return true;
            jsTestLog("Awaiting oplog truncation: number of oplog entries: " +
                      `(primary: ${numInsertOplogEntryPrimary}, ` +
                      `secondary: ${numInsertOplogEntrySecondary}) ` +
                      `number inserted: ${numInserted}`);
            return false;
        }, "Timeout waiting for oplog to roll over", ReplSetTest.kDefaultTimeoutMS, 1000);
    }

    replSet.stopSet();
}

doTest("wiredTiger");

if (jsTest.options().storageEngine !== "inMemory") {
    jsTestLog(
        "Skipping inMemory test because inMemory storageEngine was not compiled into the server.");
    return;
}

doTest("inMemory");
})();
