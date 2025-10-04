/**
 * Test that oplog (on both primary and secondary) rolls over when its size exceeds the configured
 * maximum, with parameters for setting the initial sync method and the storage engine.
 */
import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

function numInsertOplogEntry(oplog) {
    print(
        `Oplog times for ${oplog.getMongo().host}: ${tojsononeline(
            oplog.find().projection({ts: 1, t: 1, op: 1, ns: 1}).toArray(),
        )}`,
    );
    let result;
    assert.soon(() => {
        try {
            result = oplog.find({op: "i", "ns": "test.foo"}).itcount();
            return true;
        } catch (e) {
            if (e.code !== ErrorCodes.CappedPositionLost) {
                throw e;
            }
            // If we get a CappedPositionLost here, it means the oplog collection is being
            // truncated so we return false and retry.
            return false;
        }
    }, "Timeout finding the number of insert oplog entry");
    return result;
}

// Given a repl set with a primary and a secondary, inserts enough data
// to overflow the oplog(s) of any nodes that have a maximum oplog size of 1MB.
// Assumes the `hangOplogCapMaintainerThread` thread failpoint was enabled on both thee
// primary and secondary at startup.
export function rollOver1MBOplog(replSet) {
    const primary = replSet.getPrimary();
    const primaryOplog = primary.getDB("local").oplog.rs;
    const secondary = replSet.getSecondary();
    const secondaryOplog = secondary.getDB("local").oplog.rs;

    // Verify that the oplog cap maintainer thread is paused.
    assert.commandWorked(
        primary.adminCommand({
            waitForFailPoint: "hangOplogCapMaintainerThread",
            timesEntered: 1,
            maxTimeMS: kDefaultWaitForFailPointTimeout,
        }),
    );
    assert.commandWorked(
        secondary.adminCommand({
            waitForFailPoint: "hangOplogCapMaintainerThread",
            timesEntered: 1,
            maxTimeMS: kDefaultWaitForFailPointTimeout,
        }),
    );

    const coll = primary.getDB("test").foo;
    // 400KB each so that oplog can keep at most two insert oplog entries.
    const longString = "a".repeat(400 * 1024);

    // Insert the first document.
    const firstInsertTimestamp = assert.commandWorked(
        coll.runCommand("insert", {documents: [{_id: 0, longString: longString}], writeConcern: {w: 2}}),
    ).operationTime;
    jsTestLog("First insert timestamp: " + tojson(firstInsertTimestamp));

    // Test that oplog entry of the first insert exists on both primary and secondary.
    assert.eq(1, numInsertOplogEntry(primaryOplog));
    assert.eq(1, numInsertOplogEntry(secondaryOplog));

    // Insert the second document.
    const secondInsertTimestamp = assert.commandWorked(
        coll.runCommand("insert", {documents: [{_id: 1, longString: longString}], writeConcern: {w: 2}}),
    ).operationTime;
    jsTestLog("Second insert timestamp: " + tojson(secondInsertTimestamp));

    // Test that oplog entries of both inserts exist on both primary and secondary.
    assert.eq(2, numInsertOplogEntry(primaryOplog));
    assert.eq(2, numInsertOplogEntry(secondaryOplog));

    const awaitCheckpointer = function (timestamp) {
        replSet.waitForCheckpoint(primary, timestamp);
        replSet.waitForCheckpoint(secondary, timestamp);
    };

    // Wait for checkpointing/stable timestamp to catch up with the second insert so oplog
    // entry of the first insert is allowed to be deleted by the oplog cap maintainer thread
    // when a new oplog truncate marker is created. "inMemory" WT engine does not run checkpoint
    // thread and lastStableRecoveryTimestamp is the stable timestamp in this case.
    awaitCheckpointer(secondInsertTimestamp);

    // Insert the third document which will trigger a new oplog truncate marker to be created.
    // The oplog cap maintainer thread will then be unblocked on the creation of the new oplog
    // marker and will start truncating oplog entries. The oplog entry for the first
    // insert will be truncated after the oplog cap maintainer thread finishes.
    const thirdInsertTimestamp = assert.commandWorked(
        coll.runCommand("insert", {documents: [{_id: 2, longString: longString}], writeConcern: {w: 2}}),
    ).operationTime;
    jsTestLog("Third insert timestamp: " + tojson(thirdInsertTimestamp));

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
    assert.commandWorked(primary.adminCommand({configureFailPoint: "hangOplogCapMaintainerThread", mode: "off"}));
    assert.commandWorked(secondary.adminCommand({configureFailPoint: "hangOplogCapMaintainerThread", mode: "off"}));
}

export function oplogRolloverTest(storageEngine, initialSyncMethod) {
    jsTestLog("Testing with storageEngine: " + storageEngine);
    if (initialSyncMethod) {
        jsTestLog("  and initial sync method: " + initialSyncMethod);
    }

    // Pause the oplog cap maintainer thread for this test until oplog truncation is needed. The
    // truncation thread can hold a mutex for a short period of time which prevents new oplog
    // truncate markers from being created during an insertion if the mutex cannot be obtained
    // immediately. Instead, the next insertion will attempt to create a new oplog truncate marker,
    // which this test does not do.
    let parameters = {
        logComponentVerbosity: tojson({storage: 2}),
        "failpoint.hangOplogCapMaintainerThread": tojson({mode: "alwaysOn"}),
    };
    if (initialSyncMethod) {
        parameters = Object.merge(parameters, {initialSyncMethod: initialSyncMethod});
    }

    let replSetOptions = {
        // Set the syncdelay to 1s to speed up checkpointing.
        nodeOptions: {
            syncdelay: 1,
            setParameter: parameters,
        },
        nodes: [{}, {rsConfig: {priority: 0, votes: 0}}],
    };

    const replSet = new ReplSetTest(replSetOptions);
    // Set max oplog size to 1MB.
    replSet.startSet({storageEngine: storageEngine, oplogSize: 1});
    replSet.initiate();

    const primary = replSet.getPrimary();
    const primaryOplog = primary.getDB("local").oplog.rs;
    const secondary = replSet.getSecondary();
    const secondaryOplog = secondary.getDB("local").oplog.rs;

    rollOver1MBOplog(replSet);

    // Test that oplog entry of the initial insert rolls over on both primary and secondary.
    // Use assert.soon to wait for oplog cap maintainer thread to run.
    assert.soon(() => {
        return numInsertOplogEntry(primaryOplog) === 2;
    }, "Timeout waiting for oplog to roll over on primary");
    assert.soon(() => {
        return numInsertOplogEntry(secondaryOplog) === 2;
    }, "Timeout waiting for oplog to roll over on secondary");

    const res = primary.getDB("test").runCommand({serverStatus: 1});
    assert.commandWorked(res);
    assert.eq(res.oplogTruncation.truncateCount, 1, tojson(res.oplogTruncation));
    assert.gt(res.oplogTruncation.totalTimeTruncatingMicros, 0, tojson(res.oplogTruncation));

    replSet.stopSet();
}
