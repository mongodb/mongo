/**
 * Verify the FTDC metrics for mirrored reads.
 *
 * @tags: [
 *   requires_replication,
 *   requires_fcv_62
 * ]
 */
load('jstests/libs/ftdc.js');

(function() {
'use strict';

const kDbName = "mirrored_reads_ftdc_test";
const kCollName = "test";
const kOperations = 100;

const rst = new ReplSetTest({nodes: 3});
// Disable mirrored reads to make sure the initialization of oplog fetcher find commands from the
// secondaries do not get included in the metrics that we are testing.
rst.startSet({
    setParameter: {
        mirrorReads: tojsononeline({samplingRate: 0.0}),
        logComponentVerbosity: tojson({command: 1})
    }
});
rst.initiateWithHighElectionTimeout();
const primary = rst.getPrimary();
const secondaries = rst.getSecondaries();

function getDiagnosticData(node) {
    let db = node.getDB('admin');
    const stats = verifyGetDiagnosticData(db, false /* logData */).serverStatus;
    assert(stats.hasOwnProperty('mirroredReads'));
    jsTestLog(`Got diagnostic data for host: ${node}, ${tojson(stats.mirroredReads)}`);
    return stats.mirroredReads;
}

function getMirroredReadsProcessedAsSecondary() {
    let readsProcessed = 0;
    for (let i = 0; i < secondaries.length; i++) {
        const stats = getDiagnosticData(secondaries[i]);
        readsProcessed += stats.processedAsSecondary;
    }
    return readsProcessed;
}

function waitForPrimaryToSendMirroredReads(expectedReadsSeen, expectedReadsSent) {
    assert.soon(() => {
        jsTestLog("Verifying reads were seen and sent by the maestro");
        jsTestLog("ExpectedReadsSent :" + expectedReadsSent +
                  ", ExpectedReadsSeen:" + expectedReadsSeen);
        const afterPrimaryReadStats = getDiagnosticData(primary);
        const actualMirrorableReadsSeen = afterPrimaryReadStats.seen;
        const actualMirroredReadsSent = afterPrimaryReadStats.sent;
        jsTestLog("Primary metrics after reads: " + tojson(afterPrimaryReadStats));
        return expectedReadsSeen <= actualMirrorableReadsSeen &&
            expectedReadsSent == actualMirroredReadsSent;
    });
}

function sendAndCheckReads(rst) {
    const primary = rst.getPrimary();
    // Initial metrics before sending kOperations number of finds.
    const initialPrimaryReadStats = getDiagnosticData(primary);
    const mirrorableReadsSeenBefore = initialPrimaryReadStats.seen;
    const mirroredReadsSentBefore = initialPrimaryReadStats.sent;

    primary.getDB(kDbName).getCollection(kCollName).insert({x: i});
    jsTestLog(`Sending ${kOperations} reads to primary`);
    for (var i = 0; i < kOperations; ++i) {
        assert.commandWorked(primary.getDB(kDbName).runCommand({find: kCollName, filter: {}}));
    }

    const expectedReadsSeen = mirrorableReadsSeenBefore + kOperations;
    const expectedReadsSent = mirroredReadsSentBefore + (2 * kOperations);

    // Wait for primary to have sent out all mirrored reads.
    waitForPrimaryToSendMirroredReads(expectedReadsSeen, expectedReadsSent);
}

function activateFailPoint(node) {
    const db = node.getDB(kDbName);
    assert.commandWorked(db.adminCommand({
        configureFailPoint: "mirrorMaestroExpectsResponse",
        mode: "alwaysOn",
    }));
}

// Mirror every mirror-able command.
assert.commandWorked(primary.adminCommand({setParameter: 1, mirrorReads: {samplingRate: 1.0}}));
{
    // Send and check reads with mirrorMaestroExpectsResponse failpoint disabled by default.
    jsTestLog("Verifying diagnostic collection for mirrored reads on primary");
    let primaryStatsBeforeReads = getDiagnosticData(primary);
    let initialMirrorableReadsSeen = primaryStatsBeforeReads.seen;
    // The following metrics are not included by default.
    assert(!primaryStatsBeforeReads.hasOwnProperty('resolved'));
    assert(!primaryStatsBeforeReads.hasOwnProperty('resolvedBreakdown'));

    sendAndCheckReads(rst);

    assert.soon(() => {
        let mirrorableReadsSeen = getDiagnosticData(primary).seen;
        jsTestLog(`Seen ${mirrorableReadsSeen} mirrored reads so far`);
        return initialMirrorableReadsSeen + kOperations <= mirrorableReadsSeen;
    }, "Failed to update FTDC metrics within time limit", 30000);
}

{
    // Send and check reads after activating mirrorMaestroExpectsResponse fail point.
    jsTestLog("Verifying diagnostic collection when mirrorMaestroExpectsResponse");
    activateFailPoint(primary);
    assert.soon(() => {
        return getDiagnosticData(primary).hasOwnProperty('resolved');
    }, "Failed to find 'resolved' in mirrored reads FTDC metrics within time limit", 30000);

    let primaryStatsBeforeReads = getDiagnosticData(primary);
    let mirroredReadsProcessedBefore = getMirroredReadsProcessedAsSecondary();
    let primaryResolvedBeforeReads = primaryStatsBeforeReads.resolved;
    let primarySentBeforeReads = primaryStatsBeforeReads.sent;

    sendAndCheckReads(rst);

    assert.soon(() => {
        let primaryResolvedAfterReads = getDiagnosticData(primary).resolved;
        jsTestLog(`Mirrored ${primaryResolvedAfterReads} reads so far`);
        for (let i = 0; i < secondaries.length; i++) {
            // Print the secondary metrics for easier debugging.
            getDiagnosticData(secondaries[i]);
        }
        // There are two secondaries, so `kOperations * 2` reads must be resolved.
        return primaryResolvedBeforeReads + kOperations * 2 <= primaryResolvedAfterReads;
    }, "Failed to update extended FTDC metrics within time limit", 10000);

    const primaryDataAfterReads = getDiagnosticData(primary);
    const primarySentAfterReads = primaryDataAfterReads.sent;
    const primaryResolvedAfterReads = primaryDataAfterReads.resolved;
    assert.eq(primaryResolvedAfterReads - primaryResolvedBeforeReads,
              primarySentAfterReads - primarySentBeforeReads,
              primaryDataAfterReads);

    assert.soon(() => {
        jsTestLog("Verifying diagnostic collection for mirrored reads on secondaries");
        let mirroredReadsSucceeded = getDiagnosticData(primary).succeeded;
        let mirroredReadsProcessedAfter = getMirroredReadsProcessedAsSecondary();
        return mirroredReadsSucceeded ==
            (mirroredReadsProcessedAfter - mirroredReadsProcessedBefore);
    }, "Failed to wait for secondary mirrored reads stats to converge", 10000);
}
rst.stopSet();
})();
