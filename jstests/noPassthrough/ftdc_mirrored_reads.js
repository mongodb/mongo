/**
 * Verify the FTDC metrics for mirrored reads.
 *
 * @tags: [
 *   requires_replication,
 *   sbe_incompatible,
 * ]
 */
load('jstests/libs/ftdc.js');

(function() {
'use strict';

const kDbName = "mirrored_reads_ftdc_test";
const kCollName = "test";
const kOperations = 100;

function getMirroredReadsStats(rst) {
    return rst.getPrimary().getDB(kDbName).serverStatus({mirroredReads: 1}).mirroredReads;
}

function getDiagnosticData(rst) {
    let db = rst.getPrimary().getDB('admin');
    const stats = verifyGetDiagnosticData(db).serverStatus;
    assert(stats.hasOwnProperty('mirroredReads'));
    return stats.mirroredReads;
}

function sendAndCheckReads(rst) {
    let seenBeforeReads = getMirroredReadsStats(rst).seen;

    jsTestLog(`Sending ${kOperations} reads to primary`);
    for (var i = 0; i < kOperations; ++i) {
        rst.getPrimary().getDB(kDbName).runCommand({find: kCollName, filter: {}});
    }

    jsTestLog("Verifying reads were seen by the maestro");
    let seenAfterReads = getMirroredReadsStats(rst).seen;
    assert.lte(seenBeforeReads + kOperations, seenAfterReads);
}

function activateFailPoint(rst) {
    const db = rst.getPrimary().getDB(kDbName);
    assert.commandWorked(db.adminCommand({
        configureFailPoint: "mirrorMaestroExpectsResponse",
        mode: "alwaysOn",
    }));
}

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiateWithHighElectionTimeout();

// Mirror every mirror-able command.
assert.commandWorked(
    rst.getPrimary().adminCommand({setParameter: 1, mirrorReads: {samplingRate: 1.0}}));

jsTestLog("Verifying diagnostic collection for mirrored reads");
{
    let statsBeforeReads = getDiagnosticData(rst);
    // The following metrics are not included by default.
    assert(!statsBeforeReads.hasOwnProperty('resolved'));
    assert(!statsBeforeReads.hasOwnProperty('resolvedBreakdown'));

    let seenBeforeReads = statsBeforeReads.seen;
    sendAndCheckReads(rst);
    assert.soon(() => {
        let seenAfterReads = getDiagnosticData(rst).seen;
        jsTestLog(`Seen ${seenAfterReads} mirrored reads so far`);
        return seenBeforeReads + kOperations <= seenAfterReads;
    }, "Failed to update FTDC metrics within time limit", 5000);
}

jsTestLog("Verifying diagnostic collection when mirrorMaestroExpectsResponse");
{
    activateFailPoint(rst);
    assert.soon(() => {
        return getDiagnosticData(rst).hasOwnProperty('resolved');
    }, "Failed to find 'resolved' in mirrored reads FTDC metrics within time limit", 5000);
    let resolvedBeforeReads = getDiagnosticData(rst).resolved;
    sendAndCheckReads(rst);
    assert.soon(() => {
        let resolvedAfterReads = getDiagnosticData(rst).resolved;
        jsTestLog(`Mirrored ${resolvedAfterReads} reads so far`);
        // There are two secondaries, so `kOperations * 2` reads must be resolved.
        return resolvedBeforeReads + kOperations * 2 <= resolvedAfterReads;
    }, "Failed to update extended FTDC metrics within time limit", 10000);
}

rst.stopSet();
})();
