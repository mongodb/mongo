/**
 * Verify the FTDC metrics for mirrored reads.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
load('jstests/libs/ftdc.js');

(function() {
'use strict';

const kDbName = "mirrored_reads_ftdc_test";
const kCollName = "test";
const kOperations = 100;

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiateWithHighElectionTimeout();
const primary = rst.getPrimary();

function getMirroredReadsStats(node) {
    return node.getDB(kDbName).serverStatus({mirroredReads: 1}).mirroredReads;
}

function getDiagnosticData(node) {
    let db = node.getDB('admin');
    const stats = verifyGetDiagnosticData(db).serverStatus;
    assert(stats.hasOwnProperty('mirroredReads'));
    return stats.mirroredReads;
}

function waitForSecondariesToReceiveMirroredReads() {
    const mirroredReadsSent = getMirroredReadsStats(primary).sent;
    assert.soon(() => {
        jsTestLog("Verifying that secondaries received " + mirroredReadsSent +
                  " mirrored operations");
        const secondaries = rst.getSecondaries();
        // The reads received across all secondaries.
        let readsReceived = 0;
        for (let i = 0; i < secondaries.length; i++) {
            const stats = getMirroredReadsStats(secondaries[i]);
            jsTestLog("Secondary " + secondaries[i] + " metrics: " + tojson(stats));
            readsReceived += stats.received;
        }
        return readsReceived == mirroredReadsSent;
    });
}

function waitForPrimaryToSendMirroredReads(expectedReadsSeen, expectedReadsSent) {
    assert.soon(() => {
        jsTestLog("Verifying reads were seen and sent by the maestro");
        jsTestLog("ExpectedReadsSent :" + expectedReadsSent +
                  ", ExpectedReadsSeen:" + expectedReadsSeen);
        const afterPrimaryReadStats = getMirroredReadsStats(primary);
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
    const initialPrimaryReadStats = getMirroredReadsStats(primary);
    const mirrorableReadsSeenBefore = initialPrimaryReadStats.seen;
    const mirroredReadsSentBefore = initialPrimaryReadStats.sent;

    jsTestLog(`Sending ${kOperations} reads to primary`);
    for (var i = 0; i < kOperations; ++i) {
        primary.getDB(kDbName).runCommand({find: kCollName, filter: {}});
    }

    // Wait for primary to have sent out all mirrored reads.
    waitForPrimaryToSendMirroredReads(mirrorableReadsSeenBefore + kOperations,
                                      mirroredReadsSentBefore + (2 * kOperations));
    waitForSecondariesToReceiveMirroredReads();
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

jsTestLog("Verifying diagnostic collection for mirrored reads on primary");
{
    let statsBeforeReads = getDiagnosticData(primary);
    // The following metrics are not included by default.
    assert(!statsBeforeReads.hasOwnProperty('resolved'));
    assert(!statsBeforeReads.hasOwnProperty('resolvedBreakdown'));

    let initialMirrorableReadsSeen = statsBeforeReads.seen;
    sendAndCheckReads(rst);
    assert.soon(() => {
        let mirrorableReadsSeen = getDiagnosticData(primary).seen;
        jsTestLog(`Seen ${mirrorableReadsSeen} mirrored reads so far`);
        return initialMirrorableReadsSeen + kOperations <= mirrorableReadsSeen;
    }, "Failed to update FTDC metrics within time limit", 30000);
}

jsTestLog("Verifying diagnostic collection when mirrorMaestroExpectsResponse");
{
    activateFailPoint(primary);
    assert.soon(() => {
        return getDiagnosticData(primary).hasOwnProperty('resolved');
    }, "Failed to find 'resolved' in mirrored reads FTDC metrics within time limit", 30000);
    let resolvedBeforeReads = getDiagnosticData(primary).resolved;
    sendAndCheckReads(rst);
    assert.soon(() => {
        let resolvedAfterReads = getDiagnosticData(primary).resolved;
        jsTestLog(`Mirrored ${resolvedAfterReads} reads so far`);
        // There are two secondaries, so `kOperations * 2` reads must be resolved.
        return resolvedBeforeReads + kOperations * 2 <= resolvedAfterReads;
    }, "Failed to update extended FTDC metrics within time limit", 10000);
}

jsTestLog("Verifying diagnostic collection for mirrored reads on secondaries");
{
    const mirroredReadsSent = getDiagnosticData(primary).sent;
    let mirroredReadsReceived = 0;
    for (const secondary of rst.getSecondaries()) {
        mirroredReadsReceived += getDiagnosticData(secondary).received;
    }
    assert.eq(mirroredReadsSent, mirroredReadsReceived);
}

rst.stopSet();
})();
