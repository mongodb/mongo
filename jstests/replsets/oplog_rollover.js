/**
 * Test that oplog (on both primary and secondary) rolls over when its size exceeds the configured
 * maximum. This test runs on wiredTiger storage engine and inMemory storage engine (if available).
*/
(function() {
    "use strict";

    function doTest(storageEngine) {
        jsTestLog("Testing with storageEngine: " + storageEngine);

        const replSet = new ReplSetTest({
            // Set the syncdelay to 1s to speed up checkpointing.
            nodeOptions: {syncdelay: 1},
            nodes: [{}, {rsConfig: {priority: 0, votes: 0}}]
        });
        // Set max oplog size to 1MB.
        replSet.startSet({storageEngine: storageEngine, oplogSize: 1});
        replSet.initiate();

        const primary = replSet.getPrimary();
        const primaryOplog = primary.getDB("local").oplog.rs;
        const secondary = replSet.getSecondary();
        const secondaryOplog = secondary.getDB("local").oplog.rs;

        const coll = primary.getDB("test").foo;
        // 400KB each so that oplog can keep at most two insert oplog entries.
        const longString = new Array(400 * 1024).join("a");

        function numInsertOplogEntry(oplog) {
            return oplog.find({op: "i", "ns": "test.foo"}).itcount();
        }

        // Insert the first document.
        assert.commandWorked(coll.insert({_id: 0, longString: longString}, {writeConcern: {w: 2}}));
        // Test that oplog entry of the first insert exists on both primary and secondary.
        assert.eq(1, numInsertOplogEntry(primaryOplog));
        assert.eq(1, numInsertOplogEntry(secondaryOplog));

        // Insert the second document.
        const secondInsertTimestamp =
            assert
                .commandWorked(coll.runCommand(
                    "insert",
                    {documents: [{_id: 1, longString: longString}], writeConcern: {w: 2}}))
                .operationTime;
        // Test that oplog entries of both inserts exist on both primary and secondary.
        assert.eq(2, numInsertOplogEntry(primaryOplog));
        assert.eq(2, numInsertOplogEntry(secondaryOplog));

        // Have a more fine-grained test for enableMajorityReadConcern=true to also test oplog
        // truncation happens at the time we expect it to happen. When
        // enableMajorityReadConcern=false the lastStableRecoveryTimestamp is not available, so
        // switch to a coarser-grained mode to only test that oplog truncation will eventually
        // happen when oplog size exceeds the configured maximum.
        if (primary.getDB('admin').serverStatus().storageEngine.supportsCommittedReads) {
            // Wait for checkpointing/stable timestamp to catch up with the second insert so oplog
            // entry of the first insert is allowed to be deleted by the oplog cap maintainer thread
            // when a new oplog stone is created. "inMemory" WT engine does not run checkpoint
            // thread and lastStableRecoveryTimestamp is the stable timestamp in this case.
            assert.soon(
                () => {
                    const primaryTimestamp =
                        assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}))
                            .lastStableRecoveryTimestamp;
                    const secondaryTimestamp =
                        assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}))
                            .lastStableRecoveryTimestamp;
                    if (primaryTimestamp >= secondInsertTimestamp &&
                        secondaryTimestamp >= secondInsertTimestamp) {
                        return true;
                    } else {
                        jsTestLog(
                            "Awaiting last stable recovery timestamp " +
                            `(primary: ${primaryTimestamp}, secondary: ${secondaryTimestamp}) ` +
                            `target: ${secondInsertTimestamp}`);
                        return false;
                    }
                },
                "Timeout waiting for checkpointing to catch up with the second insert",
                ReplSetTest.kDefaultTimeoutMS,
                2000);

            // Insert the third document which will trigger a new oplog stone to be created. The
            // oplog cap maintainer thread will then be unblocked on the creation of the new oplog
            // stone and will start truncating oplog entries. The oplog entry for the first
            // insert will be truncated after the oplog cap maintainer thread finishes.
            assert.commandWorked(
                coll.insert({_id: 2, longString: longString}, {writeConcern: {w: 2}}));

            // Test that oplog entry of the initial insert rolls over on both primary and secondary.
            // Use assert.soon to wait for oplog cap maintainer thread to run.
            assert.soon(() => {
                return numInsertOplogEntry(primaryOplog) === 2;
            }, "Timeout waiting for oplog to roll over on primary");
            assert.soon(() => {
                return numInsertOplogEntry(secondaryOplog) === 2;
            }, "Timeout waiting for oplog to roll over on secondary");
        } else {
            // Only test that oplog truncation will eventually happen.
            let numInserted = 2;
            assert.soon(function() {
                // Insert more documents.
                assert.commandWorked(coll.insert({_id: numInserted++, longString: longString},
                                                 {writeConcern: {w: 2}}));
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
