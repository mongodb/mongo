/**
 * Verify that mirroredReads happen in response to setParameters.mirroredReads
 *
 * @tags: [
 *   requires_replication,
 *   requires_fcv_62
 * ]
 */

(function() {
"use strict";

function setParameter({rst, value}) {
    return rst.getPrimary().adminCommand({setParameter: 1, mirrorReads: value});
}

const kBurstCount = 1000;
const kDbName = "mirrored_reads_test";
const kCollName = "test";
// We use an arbitrarily large maxTimeMS to avoid timing out when processing the mirrored read
// on slower builds. Otherwise, on slower builds, the primary.mirroredReads.sent metric could
// be incremented but not the secondary.mirroredReads.processedAsSecondary metric.
const kLargeMaxTimeMS = 100000000;

function getMirroredReadsStats(node) {
    return node.getDB(kDbName).serverStatus({mirroredReads: 1}).mirroredReads;
}

function sendAndCheckReads({rst, cmd, minRate, maxRate, burstCount}) {
    const primary = rst.getPrimary();
    const secondaries = rst.getSecondaries();
    let initialPrimaryStats = getMirroredReadsStats(primary);
    let initialProcessedAsSecondary = [];
    for (const secondary of rst.getSecondaries()) {
        let secondaryMirroredReadsProcessed = getMirroredReadsStats(secondary).processedAsSecondary;
        initialProcessedAsSecondary.push(secondaryMirroredReadsProcessed);
    }

    jsTestLog(`Sending ${burstCount} request burst of ${tojson(cmd)} to primary`);

    for (var i = 0; i < burstCount; ++i) {
        rst.getPrimary().getDB(kDbName).runCommand(cmd);
    }

    jsTestLog(`Verifying ${tojson(cmd)} was mirrored`);

    // Verify that the commands have been observed on the primary
    {
        const currentPrimaryStats = getMirroredReadsStats(primary);
        assert.lte(initialPrimaryStats.seen + burstCount, currentPrimaryStats.seen);
    }

    // Verify that the reads mirrored to the secondaries have responded and secondaries receive the
    // same amount of mirrored reads that were sent by the primary.
    let currentPrimaryMirroredReadsStats;
    let readsSent;
    let readsSucceeded;
    assert.soon(() => {
        currentPrimaryMirroredReadsStats = getMirroredReadsStats(primary);
        readsSent = currentPrimaryMirroredReadsStats.sent - initialPrimaryStats.sent;
        let readsResolved =
            currentPrimaryMirroredReadsStats.resolved - initialPrimaryStats.resolved;
        readsSucceeded = currentPrimaryMirroredReadsStats.succeeded - initialPrimaryStats.succeeded;
        // The number of reads the primary has decided to mirror to secondaries, but hasn't yet
        // sent.
        let readsPending = currentPrimaryMirroredReadsStats.pending;
        let readsSeen = currentPrimaryMirroredReadsStats.seen - initialPrimaryStats.seen;

        jsTestLog("Verifying that all mirrored reads sent from primary have been resolved: " +
                  tojson({
                      sent: readsSent,
                      resolved: readsResolved,
                      succeeded: readsSucceeded,
                      pending: readsPending,
                      seen: readsSeen
                  }));
        return ((readsPending == 0) && (readsSent === readsResolved));
    }, "Did not resolve all requests within time limit", 10000);

    // The number of mirrored reads processed across all secondaries.
    let readsProcessedAsSecondaryTotal = 0;
    for (let i = 0; i < secondaries.length; i++) {
        const currentSecondaryMirroredReadsStats = getMirroredReadsStats(secondaries[i]);
        const processedAsSecondary = currentSecondaryMirroredReadsStats.processedAsSecondary -
            initialProcessedAsSecondary[i];
        jsTestLog("Verifying number of reads processed by secondary " + secondaries[i] + ": " +
                  tojson({processedAsSecondary: processedAsSecondary}));
        readsProcessedAsSecondaryTotal += processedAsSecondary;
    }
    assert.eq(readsProcessedAsSecondaryTotal, readsSucceeded);
    assert.eq(readsProcessedAsSecondaryTotal, readsSent);

    jsTestLog("Verifying primary statistics: " +
              tojson({current: currentPrimaryMirroredReadsStats, start: initialPrimaryStats}));

    let readsSeen = currentPrimaryMirroredReadsStats.seen - initialPrimaryStats.seen;
    let readsMirrored = currentPrimaryMirroredReadsStats.resolved - initialPrimaryStats.resolved;
    let numNodes = secondaries.length;
    let rate = readsMirrored / readsSeen / numNodes;

    // Check that the primary has seen all the mirrored-read supporting operations we've sent it
    assert.gte(readsSeen, burstCount);
    // Check that the rate of mirroring meets the provided criteria
    assert.gte(rate, minRate);
    assert.lte(rate, maxRate);

    jsTestLog(`Verified ${tojson(cmd)} was mirrored`);
}

function verifyMirrorReads(rst, cmd) {
    {
        jsTestLog(`Verifying disabled read mirroring with ${tojson(cmd)}`);
        let samplingRate = 0.0;

        assert.commandWorked(setParameter({rst: rst, value: {samplingRate: samplingRate}}));
        sendAndCheckReads({
            rst: rst,
            cmd: cmd,
            minRate: samplingRate,
            maxRate: samplingRate,
            burstCount: kBurstCount
        });
    }

    {
        jsTestLog(`Verifying full read mirroring with ${tojson(cmd)}`);
        let samplingRate = 1.0;

        assert.commandWorked(setParameter({rst: rst, value: {samplingRate: samplingRate}}));
        sendAndCheckReads({
            rst: rst,
            cmd: cmd,
            minRate: samplingRate,
            maxRate: samplingRate,
            burstCount: kBurstCount
        });
    }

    {
        jsTestLog(`Verifying partial read mirroring with ${tojson(cmd)}`);
        let samplingRate = 0.5;
        let gaussDeviation = .34;
        let max = samplingRate + gaussDeviation;
        let min = samplingRate - gaussDeviation;

        assert.commandWorked(setParameter({rst: rst, value: {samplingRate: samplingRate}}));
        sendAndCheckReads(
            {rst: rst, cmd: cmd, minRate: min, maxRate: max, burstCount: kBurstCount});
    }
}

function verifyProcessedAsSecondary(rst) {
    // Mirror every mirror-able command.
    const samplingRate = 1.0;
    assert.commandWorked(setParameter({rst: rst, value: {samplingRate: samplingRate}}));

    for (const secondary of rst.getSecondaries()) {
        assert.commandWorked(secondary.getDB(kDbName).adminCommand({
            configureFailPoint: "failCommand",
            mode: "alwaysOn",
            data: {
                errorCode: ErrorCodes.MaxTimeMSExpired,
                failCommands: ["find"],
            }
        }));
    }

    // With enabled fail point, check that no commands succeed or are processed, but all are
    // resolved.
    sendAndCheckReads({
        rst: rst,
        cmd: {find: kCollName, filter: {}},
        minRate: samplingRate,
        maxRate: samplingRate,
        burstCount: kBurstCount
    });

    for (const secondary of rst.getSecondaries()) {
        assert.commandWorked(secondary.getDB(kDbName).adminCommand(
            {configureFailPoint: "failCommand", mode: "off"}));
    }
}

{
    const rst = new ReplSetTest({
        nodes: 3,
        nodeOptions: {
            setParameter: {
                "failpoint.mirrorMaestroExpectsResponse": tojson({mode: "alwaysOn"}),
                "failpoint.mirrorMaestroTracksPending": tojson({mode: "alwaysOn"})
            }
        }
    });
    rst.startSet();
    rst.initiateWithHighElectionTimeout();

    jsTestLog(`Attempting invalid mirrorReads parameters`);
    assert.commandFailed(setParameter({rst: rst, value: 0.5}));
    assert.commandFailed(setParameter({rst: rst, value: "half"}));
    assert.commandFailed(setParameter({rst: rst, value: {samplingRate: -1.0}}));
    assert.commandFailed(setParameter({rst: rst, value: {samplingRate: 1.01}}));
    assert.commandFailed(setParameter({rst: rst, value: {somplingRate: 1.0}}));

    // Put in a datum
    {
        let ret =
            rst.getPrimary().getDB(kDbName).runCommand({insert: kCollName, documents: [{x: 1}]});
        assert.commandWorked(ret);
    }

    jsTestLog("Verifying mirrored reads for 'find' commands");
    verifyMirrorReads(rst, {find: kCollName, filter: {}, maxTimeMS: kLargeMaxTimeMS});

    jsTestLog("Verifying mirrored reads for 'count' commands");
    verifyMirrorReads(rst, {count: kCollName, query: {}, maxTimeMS: kLargeMaxTimeMS});

    jsTestLog("Verifying mirrored reads for 'distinct' commands");
    verifyMirrorReads(rst, {distinct: kCollName, key: "x", maxTimeMS: kLargeMaxTimeMS});

    jsTestLog("Verifying mirrored reads for 'findAndModify' commands");
    verifyMirrorReads(rst, {
        findAndModify: kCollName,
        query: {},
        update: {'$inc': {x: 1}},
        maxTimeMS: kLargeMaxTimeMS
    });

    jsTestLog("Verifying mirrored reads for 'update' commands");
    verifyMirrorReads(rst, {
        update: kCollName,
        updates: [{q: {_id: 1}, u: {'$inc': {x: 1}}}],
        ordered: false,
        maxTimeMS: kLargeMaxTimeMS
    });

    jsTestLog("Verifying processedAsSecondary field for 'find' commands");
    verifyProcessedAsSecondary(rst);

    rst.stopSet();
}

function computeMean(before, after) {
    let sum = 0;
    let count = 0;

    Object.keys(after.resolvedBreakdown).forEach(function(host) {
        sum = sum + after.resolvedBreakdown[host];
        if (host in before.resolvedBreakdown) {
            sum = sum - before.resolvedBreakdown[host];
        }
        count = count + 1;
    });

    return sum / count;
}

function computeSTD(before, after, mean) {
    let stDev = 0.0;
    let count = 0;

    Object.keys(after.resolvedBreakdown).forEach(function(host) {
        let result = after.resolvedBreakdown[host];
        if (host in before.resolvedBreakdown) {
            result = result - before.resolvedBreakdown[host];
        }
        result = result - mean;
        result = result * result;
        stDev = stDev + result;

        count = count + 1;
    });

    return Math.sqrt(stDev / count);
}

function verifyMirroringDistribution(rst) {
    let nodeCount = rst.nodes.length;

    const samplingRate = 0.5;
    const gaussDeviation = .34;
    const max = samplingRate + gaussDeviation;
    const min = samplingRate - gaussDeviation;

    jsTestLog(`Running test with sampling rate = ${samplingRate}`);
    assert.commandWorked(setParameter({rst: rst, value: {samplingRate: samplingRate}}));

    let before = getMirroredReadsStats(rst.getPrimary());

    sendAndCheckReads({
        rst: rst,
        cmd: {find: kCollName, filter: {}},
        minRate: min,
        maxRate: max,
        burstCount: kBurstCount,
        checkExpectedReadsProcessed: false,
        expectedReadsProcessed: 0
    });

    let after = getMirroredReadsStats(rst.getPrimary());

    let mean = computeMean(before, after);
    let std = computeSTD(before, after, mean);
    jsTestLog(`Mean =  ${mean}; STD = ${std}`);

    // Verify that relative standard deviation is less than 25%.
    let relativeSTD = std / mean;
    assert(relativeSTD < 0.25);
}

{
    for (var secondaries = 2; secondaries <= 4; secondaries++) {
        const rst = new ReplSetTest({
            nodes: secondaries + 1,
            nodeOptions: {
                setParameter: {
                    "failpoint.mirrorMaestroExpectsResponse": tojson({mode: "alwaysOn"}),
                    "failpoint.mirrorMaestroTracksPending": tojson({mode: "alwaysOn"})
                }
            }
        });
        rst.startSet();
        rst.initiateWithHighElectionTimeout();

        jsTestLog(`Verifying mirroring distribution for ${secondaries} secondaries`);
        verifyMirroringDistribution(rst);
        rst.stopSet();
    }
}
})();
