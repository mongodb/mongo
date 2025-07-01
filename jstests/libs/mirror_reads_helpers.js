/**
 * Helper functions for testing mirrored reads
 */

const kBurstCount = 1000;
const kGeneralMode = "general";
const kTargetedMode = "targeted";

function setParameter({nodeToReadFrom, value}) {
    return nodeToReadFrom.adminCommand({setParameter: 1, mirrorReads: value});
}

function getSecondariesWithTargetedTag(nodeToReadFrom, tag) {
    let nodeIdsWithTag = [];
    let rsConfig = nodeToReadFrom.getDB("local").system.replset.findOne();
    rsConfig.members.forEach(function(member) {
        if (bsonWoCompare(member.tags, tag) == 0) {
            nodeIdsWithTag.push(member.host);
        }
    });
    return nodeIdsWithTag;
}

function getMirroredReadsStats(node, dbName) {
    return node.getDB(dbName).serverStatus({mirroredReads: 1}).mirroredReads;
}

/* Get the differences between the before and after stats for each field. For the stats that
   accumulate, this is effectively the stats that have accumulated since beforeStats. */
function getStatDifferences(beforeStats, afterStats) {
    let differences = {};
    for (const fieldName of Object.keys(beforeStats)) {
        differences[fieldName] = afterStats[fieldName] - beforeStats[fieldName];
    }
    return differences;
}

function getStat(stats, statName, mirrorMode) {
    switch (statName) {
        case "sent":
            return (mirrorMode == kGeneralMode ? stats.sent : stats.targetedSent);
        case "succeeded":
            return (mirrorMode == kGeneralMode ? stats.succeeded : stats.targetedSucceeded);
        case "erroredDuringSend":
            return (mirrorMode == kGeneralMode ? stats.erroredDuringSend
                                               : stats.targetedErroredDuringSend);
        case "resolved":
            return (mirrorMode == kGeneralMode ? stats.resolved : stats.targetedResolved);
        case "seen":
            return stats.seen;
        case "pending":
            return (mirrorMode == kGeneralMode ? stats.pending : stats.targetedPending);
        case "scheduled":
            return (mirrorMode == kGeneralMode ? stats.scheduled : stats.targetedScheduled);
    }
}

/* Send the cmd and verify it's seen on the primary. */
function sendReads({nodeToReadFrom, db, cmd, burstCount, initialStatsOnReadingNode}) {
    jsTestLog(`Sending ${burstCount} request burst of ${tojson(cmd)} to ${nodeToReadFrom.host}`);

    for (var i = 0; i < burstCount; ++i) {
        nodeToReadFrom.getDB(db).runCommand(cmd);
    }

    jsTestLog(`Verifying ${tojson(cmd)} was mirrored`);

    // Verify that the commands have been observed on the node the client targets.
    {
        const currentStatsOnReadingNode = getMirroredReadsStats(nodeToReadFrom, db);
        assert.lte(initialStatsOnReadingNode.seen + burstCount, currentStatsOnReadingNode.seen);
    }
}

/* Wait for all reads to resolve on primary, and check various other metrics related to the
   primary. */
function waitForReadsToResolveOnTargetedNode(
    nodeToReadFrom, mirrorMode, db, initialStatsOnReadingNode, readsExpectedToFail) {
    let sent, succeeded, erroredDuringSend;

    // Wait for stats to reflect that all the sent reads have been resolved.
    assert.soon(() => {
        let currentStatsOnReadingNode = getMirroredReadsStats(nodeToReadFrom, db);
        const statDifferenceOnReadingNode =
            getStatDifferences(initialStatsOnReadingNode, currentStatsOnReadingNode);

        sent = getStat(statDifferenceOnReadingNode, "sent", mirrorMode);
        succeeded = getStat(statDifferenceOnReadingNode, "succeeded", mirrorMode);
        erroredDuringSend = getStat(statDifferenceOnReadingNode, "erroredDuringSend", mirrorMode);
        let resolved = getStat(statDifferenceOnReadingNode, "resolved", mirrorMode);
        let seen = getStat(statDifferenceOnReadingNode, "seen", mirrorMode);

        // `pending` refers to the number of reads the node should mirror, but hasn't yet
        // scheduled. Unlike the other mirrored reads metrics, this metric does not accumulate, and
        // refers only to the reads currently pending.
        let pending = getStat(statDifferenceOnReadingNode, "pending", mirrorMode);

        // `scheduled` refers to the number of reads the node has scheduled mirrors for, but
        // hasn't yet resolved. Unlike the other mirrored reads metrics, this metric does not
        // accumulate, and refers only to the reads currently scheduled.
        let scheduled = getStat(statDifferenceOnReadingNode, "scheduled", mirrorMode);

        jsTestLog(`Verifying that all mirrored reads sent from ${
                      nodeToReadFrom.host} have been resolved: ` +
                  tojson({
                      sent: sent,
                      erroredDuringSend: erroredDuringSend,
                      resolved: resolved,
                      succeeded: succeeded,
                      pending: pending,
                      scheduled: scheduled,
                      seen: seen
                  }));

        // Verify that the reads mirrored to the secondaries have been resolved by the reading node.
        return pending == 0 && scheduled == 0 && sent === resolved;
    }, "Did not resolve all requests within time limit", 20000);

    if (!readsExpectedToFail) {
        // If we don't expect to fail from some fail point, then we expect most of our reads
        // to succeed barring some transient errors.
        assert.eq(succeeded + erroredDuringSend, sent);
    }
}

function getProcessedAsSecondaryTotal(
    rst, mirrorMode, db, initialStatsOnSecondaries, secondariesWithTag) {
    const secondaries = rst.getSecondaries();

    let processedAsSecondaryTotal = 0;
    for (const secondary of secondaries) {
        const statDifferenceOnSecondary = getStatDifferences(
            initialStatsOnSecondaries[secondary.nodeId], getMirroredReadsStats(secondary, db));
        const processedAsSecondary = statDifferenceOnSecondary.processedAsSecondary;
        processedAsSecondaryTotal += processedAsSecondary;

        if (mirrorMode == kTargetedMode && !secondariesWithTag.includes(secondary.host)) {
            // This secondary should not have been targeted, so the difference in mirrored reads
            // processed should be 0.
            assert.eq(processedAsSecondary, 0);
        }
    }
    return processedAsSecondaryTotal;
}

function checkStatsOnSecondaries(rst,
                                 nodeToReadFrom,
                                 mirrorMode,
                                 db,
                                 initialStatsOnReadingNode,
                                 initialStatsOnSecondaries,
                                 currentStatsOnReadingNode,
                                 readsExpectedToFail,
                                 secondariesWithTag) {
    let statDifferenceOnReadingNode =
        getStatDifferences(initialStatsOnReadingNode, currentStatsOnReadingNode);

    let sent = getStat(statDifferenceOnReadingNode, "sent", mirrorMode);
    let erroredDuringSend = getStat(statDifferenceOnReadingNode, "erroredDuringSend", mirrorMode);
    let processedAsSecondaryTotal = getProcessedAsSecondaryTotal(
        rst, mirrorMode, db, initialStatsOnSecondaries, secondariesWithTag);

    assert.eq(processedAsSecondaryTotal + erroredDuringSend, sent);
    if (!readsExpectedToFail) {
        let succeeded = getStat(statDifferenceOnReadingNode, "succeeded", mirrorMode);
        assert.eq(processedAsSecondaryTotal, succeeded);
    }
}

/* Check that the average number of reads resolved per secondary is within the specified rates. */
function checkReadsMirroringRate({
    mirrorMode,
    cmd,
    minRate,
    maxRate,
    initialStatsOnReadingNode,
    currentStatsOnReadingNode,
    nodesElligibleForMirrors
}) {
    let statDifferenceOnReadingNode =
        getStatDifferences(initialStatsOnReadingNode, currentStatsOnReadingNode);

    let seen = getStat(statDifferenceOnReadingNode, "seen", mirrorMode);
    let resolved = getStat(statDifferenceOnReadingNode, "resolved", mirrorMode);

    let rate = resolved / seen / nodesElligibleForMirrors;
    // Check that the rate of mirroring meets the provided criteria
    assert.gte(rate, minRate);
    assert.lte(rate, maxRate);

    jsTestLog(`Verified rate of mirroring for ${tojson(cmd)}`);
}

/* Send `burstCount` mirror reads. Check metrics are values we expect when the reads succeed. */
function sendAndCheckReadsSucceedWithRate(
    {rst, nodeToReadFrom, secondariesWithTag, mirrorMode, db, cmd, minRate, maxRate, burstCount}) {
    let initialStatsOnReadingNode = getMirroredReadsStats(nodeToReadFrom, db);

    let initialStatsOnMirroredSecondaries = {};
    let secondaries = rst.getSecondaries();
    for (const secondary of secondaries) {
        initialStatsOnMirroredSecondaries[secondary.nodeId] = getMirroredReadsStats(secondary, db);
    }

    sendReads({nodeToReadFrom, db, cmd, burstCount, initialStatsOnReadingNode});
    waitForReadsToResolveOnTargetedNode(
        nodeToReadFrom, mirrorMode, db, initialStatsOnReadingNode, false);

    // Stats should be stable now that all of the reads have resolved.
    let currentStatsOnReadingNode = getMirroredReadsStats(nodeToReadFrom, db);
    jsTestLog("Verifying sending node statistics: " +
              tojson({current: currentStatsOnReadingNode, start: initialStatsOnReadingNode}));
    checkStatsOnSecondaries(rst,
                            nodeToReadFrom,
                            mirrorMode,
                            db,
                            initialStatsOnReadingNode,
                            initialStatsOnMirroredSecondaries,
                            currentStatsOnReadingNode,
                            false,
                            secondariesWithTag);

    let nodesElligibleForMirrors =
        mirrorMode == kGeneralMode ? secondaries.length : secondariesWithTag.length;
    checkReadsMirroringRate({
        mirrorMode,
        cmd,
        minRate,
        maxRate,
        initialStatsOnReadingNode,
        currentStatsOnReadingNode,
        nodesElligibleForMirrors
    });
}

/* Send `burstCount` mirror reads. Verify that the metrics reflect when the command fails before
processing on the secondaries. */
function sendAndCheckReadsFailBeforeProcessing({
    rst,
    nodeToReadFrom,
    secondariesWithTag,
    mirrorMode,
    db,
    cmd,
    burstCount,
    expectErroredDuringSend
}) {
    const secondaries = rst.getSecondaries();

    let initialStatsOnReadingNode = getMirroredReadsStats(nodeToReadFrom, db);
    let initialStatsOnSecondaries = {};
    for (const secondary of secondaries) {
        initialStatsOnSecondaries[secondary.nodeId] = getMirroredReadsStats(secondary, db);
    }

    sendReads({nodeToReadFrom, db, cmd, burstCount, initialStatsOnReadingNode});

    waitForReadsToResolveOnTargetedNode(
        nodeToReadFrom, mirrorMode, db, initialStatsOnReadingNode, true);

    // Stats should be stable now that all of the reads have resolved.
    let currentStatsOnReadingNode = getMirroredReadsStats(nodeToReadFrom, db);
    jsTestLog("Verifying sending node statistics: " +
              tojson({current: currentStatsOnReadingNode, start: initialStatsOnReadingNode}));

    let processedAsSecondaryTotal = getProcessedAsSecondaryTotal(
        rst, mirrorMode, db, initialStatsOnSecondaries, secondariesWithTag);
    let statDifferenceOnReadingNode =
        getStatDifferences(initialStatsOnReadingNode, currentStatsOnReadingNode);
    let succeeded = getStat(statDifferenceOnReadingNode, "succeeded", mirrorMode);
    let erroredDuringSend = getStat(statDifferenceOnReadingNode, "erroredDuringSend", mirrorMode);

    // Expect no mirrored reads to be processed because the fail point times out the query
    // before the processed metric is incremented.
    assert.eq(processedAsSecondaryTotal, 0);
    assert.eq(succeeded, 0);

    if (expectErroredDuringSend) {
        assert.eq(erroredDuringSend, getStat(statDifferenceOnReadingNode, "sent", mirrorMode));
    } else {
        assert.eq(erroredDuringSend, 0);
    }
}

/* Verify that the processedAsSecondary metric does not increment if the command fails
   on the secondary before the secondary is able to process the read. */
function verifyProcessedAsSecondaryOnEarlyError(rst, mirrorMode, dbName, collName) {
    let nodeToReadFrom = (mirrorMode == kGeneralMode ? rst.getPrimary() : rst.getSecondaries()[0]);

    let secondariesWithTag = [];
    if (mirrorMode == kTargetedMode) {
        secondariesWithTag = getSecondariesWithTargetedTag(nodeToReadFrom);
    }

    // Mirror every mirror-able command.
    const samplingRate = 1.0;
    let param = (mirrorMode == kGeneralMode
                     ? {samplingRate: samplingRate, targetedMirroring: {samplingRate: 0.0}}
                     : {targetedMirroring: {samplingRate: samplingRate}});
    assert.commandWorked(setParameter({nodeToReadFrom: nodeToReadFrom, value: param}));

    for (const secondary of rst.getSecondaries()) {
        if (secondary == nodeToReadFrom) {
            continue;
        }

        assert.commandWorked(secondary.getDB(dbName).adminCommand({
            configureFailPoint: "failCommand",
            mode: "alwaysOn",
            data: {
                errorCode: ErrorCodes.MaxTimeMSExpired,
                failCommands: ["find"],
                failInternalCommands: true,
            }
        }));
    }

    sendAndCheckReadsFailBeforeProcessing({
        rst: rst,
        nodeToReadFrom: nodeToReadFrom,
        secondariesWithTag: secondariesWithTag,
        mirrorMode: mirrorMode,
        db: dbName,
        cmd: {find: collName, filter: {}},
        burstCount: kBurstCount,
        expectErroredDuringSend: false
    });

    for (const secondary of rst.getSecondaries()) {
        if (secondary == nodeToReadFrom) {
            continue;
        }

        assert.commandWorked(
            secondary.getDB(dbName).adminCommand({configureFailPoint: "failCommand", mode: "off"}));
    }
}

function verifyErroredDuringSend(rst, mirrorMode, dbName, collName, tag) {
    let nodeToReadFrom = (mirrorMode == kGeneralMode ? rst.getPrimary() : rst.getSecondaries()[0]);

    let secondariesWithTag = [];
    if (mirrorMode == kTargetedMode) {
        secondariesWithTag = getSecondariesWithTargetedTag(nodeToReadFrom, tag);
    }

    // Mirror every mirror-able command.
    const samplingRate = 1.0;
    let param = (mirrorMode == kGeneralMode
                     ? {samplingRate: samplingRate, targetedMirroring: {samplingRate: 0.0}}
                     : {targetedMirroring: {samplingRate: samplingRate, tag: tag}});
    assert.commandWorked(setParameter({nodeToReadFrom: nodeToReadFrom, value: param}));

    assert.commandWorked(nodeToReadFrom.getDB(dbName).adminCommand({
        configureFailPoint: "forceConnectionNetworkTimeout",
        mode: "alwaysOn",
        data: {
            collectionNS: collName,
        }
    }));

    sendAndCheckReadsFailBeforeProcessing({
        rst: rst,
        nodeToReadFrom: nodeToReadFrom,
        secondariesWithTag: secondariesWithTag,
        mirrorMode: mirrorMode,
        db: dbName,
        cmd: {find: collName, filter: {}},
        burstCount: kBurstCount,
        expectErroredDuringSend: true
    });

    assert.commandWorked(nodeToReadFrom.getDB(dbName).adminCommand(
        {configureFailPoint: "forceConnectionNetworkTimeout", mode: "off"}));
}

/* Verify mirror reads behavior with various sampling rates. */
function verifyMirrorReads(rst, mirrorMode, db, cmd, tag) {
    let nodeToReadFrom = (mirrorMode == kGeneralMode ? rst.getPrimary() : rst.getSecondaries()[0]);
    let secondariesWithTag = [];
    if (mirrorMode == kTargetedMode) {
        secondariesWithTag = getSecondariesWithTargetedTag(nodeToReadFrom, tag);
    }

    {
        jsTestLog(`Verifying disabled read mirroring with ${tojson(cmd)}`);
        let samplingRate = 0.0;
        let param = (mirrorMode == kGeneralMode
                         ? {samplingRate: samplingRate, targetedMirroring: {samplingRate: 0.0}}
                         : {targetedMirroring: {samplingRate: samplingRate, tag: tag}});
        assert.commandWorked(setParameter({nodeToReadFrom: nodeToReadFrom, value: param}));
        sendAndCheckReadsSucceedWithRate({
            rst: rst,
            nodeToReadFrom: nodeToReadFrom,
            secondariesWithTag: secondariesWithTag,
            mirrorMode: mirrorMode,
            db: db,
            cmd: cmd,
            minRate: samplingRate,
            maxRate: samplingRate,
            burstCount: kBurstCount
        });
    }

    {
        jsTestLog(`Verifying full read mirroring with ${tojson(cmd)}`);
        let samplingRate = 1.0;
        let param = (mirrorMode == kGeneralMode
                         ? {samplingRate: samplingRate, targetedMirroring: {samplingRate: 0.0}}
                         : {targetedMirroring: {samplingRate: samplingRate, tag: tag}});

        assert.commandWorked(setParameter({nodeToReadFrom: nodeToReadFrom, value: param}));
        sendAndCheckReadsSucceedWithRate({
            rst: rst,
            nodeToReadFrom: nodeToReadFrom,
            secondariesWithTag: secondariesWithTag,
            mirrorMode: mirrorMode,
            db: db,
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

        let param = (mirrorMode == kGeneralMode
                         ? {samplingRate: samplingRate, targetedMirroring: {samplingRate: 0.0}}
                         : {targetedMirroring: {samplingRate: samplingRate, tag: tag}});
        assert.commandWorked(setParameter({nodeToReadFrom: nodeToReadFrom, value: param}));
        sendAndCheckReadsSucceedWithRate({
            rst: rst,
            nodeToReadFrom: nodeToReadFrom,
            secondariesWithTag: secondariesWithTag,
            mirrorMode: mirrorMode,
            db: db,
            cmd: cmd,
            minRate: min,
            maxRate: max,
            burstCount: kBurstCount
        });
    }

    // Reset param to avoid mirroring before next test case
    let samplingRate = 0.0;
    let param = (mirrorMode == kGeneralMode
                     ? {samplingRate: samplingRate, targetedMirroring: {samplingRate: 0.0}}
                     : {targetedMirroring: {samplingRate: samplingRate, tag: tag}});
    assert.commandWorked(setParameter({nodeToReadFrom: nodeToReadFrom, value: param}));
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

/* Verify that mirrored reads are distributed reasonably across secondaries */
function verifyMirroringDistribution(rst, mirrorMode, dbName, collName) {
    const samplingRate = 0.5;
    const gaussDeviation = .34;
    const max = samplingRate + gaussDeviation;
    const min = samplingRate - gaussDeviation;

    let nodeToReadFrom = (mirrorMode == kGeneralMode ? rst.getPrimary() : rst.getSecondaries()[0]);

    jsTestLog(`Running test with sampling rate = ${samplingRate}`);
    let param = (mirrorMode == kGeneralMode
                     ? {samplingRate: samplingRate, targetedMirroring: {samplingRate: 0.0}}
                     : {targetedMirroring: {samplingRate: samplingRate}});
    assert.commandWorked(setParameter({nodeToReadFrom: nodeToReadFrom, value: param}));

    let before = getMirroredReadsStats(nodeToReadFrom, dbName);

    sendAndCheckReadsSucceedWithRate({
        rst: rst,
        nodeToReadFrom: nodeToReadFrom,
        mirrorMode: mirrorMode,
        db: dbName,
        cmd: {find: collName, filter: {}},
        minRate: min,
        maxRate: max,
        burstCount: kBurstCount,
    });

    let after = getMirroredReadsStats(nodeToReadFrom, dbName);

    let mean = computeMean(before, after);
    let std = computeSTD(before, after, mean);
    jsTestLog(`Mean =  ${mean}; STD = ${std}`);

    // Verify that relative standard deviation is less than 25%.
    let relativeSTD = std / mean;
    assert(relativeSTD < 0.25);
}

export const MirrorReadsHelpers = {
    kBurstCount,
    kGeneralMode,
    kTargetedMode,
    setParameter,
    getSecondariesWithTargetedTag,
    getMirroredReadsStats,
    getStatDifferences,
    sendReads,
    sendAndCheckReadsSucceedWithRate,
    verifyProcessedAsSecondaryOnEarlyError,
    verifyErroredDuringSend,
    verifyMirrorReads,
    verifyMirroringDistribution
};
