/**
 * Verify that mirroredReads happen in response to setParameters.mirroredReads
 *
 * @tags: [
 *   requires_replication,
 *   requires_fcv_62
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

function setParameter({rst, value}) {
    return rst.getPrimary().adminCommand({setParameter: 1, mirrorReads: value});
}

const kBurstCount = 1000;
const kDbName = "mirrored_reads_test";
const kCollName = "coll";
// We use an arbitrarily large maxTimeMS to avoid timing out when processing the mirrored read
// on slower builds. We want to give the secondary.mirroredReads.processedAsSecondary metric as
// much time as possible to increment.
const kLargeMaxTimeMS = 100000000;

function getMirroredReadsStats(node) {
    return node.getDB(kDbName).serverStatus({mirroredReads: 1}).mirroredReads;
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

/* Send the cmd and verify it's seen on the primary. */
function sendReads({rst, db, cmd, burstCount, initialStatsOnPrimary}) {
    const primary = rst.getPrimary();

    jsTestLog(`Sending ${burstCount} request burst of ${tojson(cmd)} to primary`);

    for (var i = 0; i < burstCount; ++i) {
        primary.getDB(db).runCommand(cmd);
    }

    jsTestLog(`Verifying ${tojson(cmd)} was mirrored`);

    // Verify that the commands have been observed on the primary.
    {
        const currentStatsOnPrimary = getMirroredReadsStats(primary);
        assert.lte(initialStatsOnPrimary.seen + burstCount, currentStatsOnPrimary.seen);
    }
}

/* Wait for all reads to resolve on primary, and check various other metrics related to the
   primary. */
function waitForReadsToResolveOnPrimary(rst, initialStatsOnPrimary, readsExpectedToFail) {
    let primary = rst.getPrimary();
    let sent, succeeded, erroredDuringSend;
    // Wait for stats to reflect that all the sent reads have been resolved.
    assert.soon(() => {
        let currentStatsOnPrimary = getMirroredReadsStats(primary);
        const statDifferenceOnPrimary =
            getStatDifferences(initialStatsOnPrimary, currentStatsOnPrimary);
        sent = statDifferenceOnPrimary.sent;
        succeeded = statDifferenceOnPrimary.succeeded;
        erroredDuringSend = statDifferenceOnPrimary.erroredDuringSend;
        let resolved = statDifferenceOnPrimary.resolved;
        let seen = statDifferenceOnPrimary.seen;
        // `pending` refers to the number of reads the primary has decided to mirror to
        // secondaries, but hasn't yet resolved. Unlike the other mirrored reads metrics, this
        // metric does not accumulate, and refers only to the reads currently pending.
        let pending = currentStatsOnPrimary.pending;

        jsTestLog("Verifying that all mirrored reads sent from primary have been resolved: " +
                  tojson({
                      sent: sent,
                      erroredDuringSend: erroredDuringSend,
                      resolved: resolved,
                      succeeded: succeeded,
                      pending: pending,
                      seen: seen
                  }));
        // Verify that the reads mirrored to the secondaries have been resolved by the primary.
        return ((pending == 0) && (sent === resolved));
    }, "Did not resolve all requests within time limit", 20000);

    if (!readsExpectedToFail) {
        // If we don't expect to fail from some fail point, then we expect most of our reads
        // to succeed barring some transient errors.
        assert.eq(succeeded + erroredDuringSend, sent);
    }
}

function getProcessedAsSecondaryTotal(rst, initialStatsOnSecondaries) {
    const secondaries = rst.getSecondaries();
    let processedAsSecondaryTotal = 0;
    for (const secondary of secondaries) {
        const statDifferenceOnSecondary = getStatDifferences(
            initialStatsOnSecondaries[secondary.nodeId], getMirroredReadsStats(secondary));
        const processedAsSecondary = statDifferenceOnSecondary.processedAsSecondary;
        processedAsSecondaryTotal += processedAsSecondary;
    }
    return processedAsSecondaryTotal;
}

function checkStatsOnSecondaries(rst,
                                 initialStatsOnPrimary,
                                 initialStatsOnSecondaries,
                                 currentStatsOnPrimary,
                                 readsExpectedToFail) {
    let statDifferenceOnPrimary = getStatDifferences(initialStatsOnPrimary, currentStatsOnPrimary);
    let sent = statDifferenceOnPrimary.sent;
    let erroredDuringSend = statDifferenceOnPrimary.erroredDuringSend;
    let processedAsSecondaryTotal = getProcessedAsSecondaryTotal(rst, initialStatsOnSecondaries);
    assert.eq(processedAsSecondaryTotal + erroredDuringSend, sent);
    if (!readsExpectedToFail) {
        let succeeded = statDifferenceOnPrimary.succeeded;
        assert.eq(processedAsSecondaryTotal, succeeded);
    }
}

/* Check that the average number of reads resolved per secondary is within the specified rates. */
function checkReadsMirroringRate(
    {rst, cmd, minRate, maxRate, initialStatsOnPrimary, currentStatsOnPrimary}) {
    let statDifferenceOnPrimary = getStatDifferences(initialStatsOnPrimary, currentStatsOnPrimary);
    let seen = statDifferenceOnPrimary.seen;
    let resolved = statDifferenceOnPrimary.resolved;

    let numNodes = rst.getSecondaries().length;
    let rate = resolved / seen / numNodes;
    // Check that the rate of mirroring meets the provided criteria
    assert.gte(rate, minRate);
    assert.lte(rate, maxRate);

    jsTestLog(`Verified rate of mirroring for ${tojson(cmd)}`);
}

/* Send `burstCount` mirror reads. Check metrics are values we expect when the reads succeed. */
function sendAndCheckReadsSucceedWithRate({rst, db, cmd, minRate, maxRate, burstCount}) {
    const primary = rst.getPrimary();
    const secondaries = rst.getSecondaries();

    let initialStatsOnPrimary = getMirroredReadsStats(primary);
    let initialStatsOnSecondaries = {};
    for (const secondary of secondaries) {
        initialStatsOnSecondaries[secondary.nodeId] = (getMirroredReadsStats(secondary));
    }

    sendReads({rst, db, cmd, burstCount, initialStatsOnPrimary});

    waitForReadsToResolveOnPrimary(rst, initialStatsOnPrimary, false);

    // Stats should be stable now that all of the reads have resolved.
    let currentStatsOnPrimary = getMirroredReadsStats(primary);
    jsTestLog("Verifying primary statistics: " +
              tojson({current: currentStatsOnPrimary, start: initialStatsOnPrimary}));

    checkStatsOnSecondaries(
        rst, initialStatsOnPrimary, initialStatsOnSecondaries, currentStatsOnPrimary, false);
    checkReadsMirroringRate(
        {rst, cmd, minRate, maxRate, initialStatsOnPrimary, currentStatsOnPrimary});
}

/* Send `burstCount` mirror reads. Verify that the metrics when the command fail before processing
   on the secondaries. */
function sendAndCheckReadsFailBeforeProcessing(
    {rst, db, cmd, burstCount, expectErroredDuringSend}) {
    const primary = rst.getPrimary();
    const secondaries = rst.getSecondaries();

    let initialStatsOnPrimary = getMirroredReadsStats(primary);
    let initialStatsOnSecondaries = {};
    for (const secondary of secondaries) {
        initialStatsOnSecondaries[secondary.nodeId] = (getMirroredReadsStats(secondary));
    }

    sendReads({rst, db, cmd, burstCount, initialStatsOnPrimary});

    waitForReadsToResolveOnPrimary(rst, initialStatsOnPrimary, true);

    // Stats should be stable now that all of the reads have resolved.
    let currentStatsOnPrimary = getMirroredReadsStats(primary);
    jsTestLog("Verifying primary statistics: " +
              tojson({current: currentStatsOnPrimary, start: initialStatsOnPrimary}));

    let processedAsSecondaryTotal = getProcessedAsSecondaryTotal(rst, initialStatsOnSecondaries);
    let statDifferenceOnPrimary = getStatDifferences(initialStatsOnPrimary, currentStatsOnPrimary);
    let succeeded = statDifferenceOnPrimary.succeeded;
    let erroredDuringSend = statDifferenceOnPrimary.erroredDuringSend;

    // Expect no mirrored reads to be processed because the fail point times out the query
    // before the processed metric is incremented.
    assert.eq(processedAsSecondaryTotal, 0);
    assert.eq(succeeded, 0);

    if (expectErroredDuringSend) {
        assert.eq(erroredDuringSend, statDifferenceOnPrimary.sent);
    } else {
        assert.eq(erroredDuringSend, 0);
    }
}

/* Verify that the processedAsSecondary metric does not increment if the command fails
   on the secondary before the secondary is able to process the read. */
function verifyProcessedAsSecondaryOnEarlyError(rst) {
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
                failInternalCommands: true,
            }
        }));
    }

    sendAndCheckReadsFailBeforeProcessing({
        rst: rst,
        db: kDbName,
        cmd: {find: kCollName, filter: {}},
        burstCount: kBurstCount,
        expectErroredDuringSend: false
    });

    for (const secondary of rst.getSecondaries()) {
        assert.commandWorked(secondary.getDB(kDbName).adminCommand(
            {configureFailPoint: "failCommand", mode: "off"}));
    }
}

/* Verify that the erroredDuringSend metric increments if the command fails . */
function verifyErroredDuringSend(rst) {
    // Mirror every mirror-able command.
    const samplingRate = 1.0;
    assert.commandWorked(setParameter({rst: rst, value: {samplingRate: samplingRate}}));

    assert.commandWorked(rst.getPrimary().getDB(kDbName).adminCommand({
        configureFailPoint: "forceConnectionNetworkTimeout",
        mode: "alwaysOn",
        data: {
            collectionNS: kCollName,
        }
    }));

    sendAndCheckReadsFailBeforeProcessing({
        rst: rst,
        db: kDbName,
        cmd: {find: kCollName, filter: {}},
        burstCount: kBurstCount,
        expectErroredDuringSend: true
    });

    assert.commandWorked(rst.getPrimary().getDB(kDbName).adminCommand(
        {configureFailPoint: "forceConnectionNetworkTimeout", mode: "off"}));
}

/* Verify mirror reads behavior with various sampling rates. */
function verifyMirrorReads(rst, db, cmd) {
    {
        jsTestLog(`Verifying disabled read mirroring with ${tojson(cmd)}`);
        let samplingRate = 0.0;

        assert.commandWorked(setParameter({rst: rst, value: {samplingRate: samplingRate}}));
        sendAndCheckReadsSucceedWithRate({
            rst: rst,
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

        assert.commandWorked(setParameter({rst: rst, value: {samplingRate: samplingRate}}));
        sendAndCheckReadsSucceedWithRate({
            rst: rst,
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

        assert.commandWorked(setParameter({rst: rst, value: {samplingRate: samplingRate}}));
        sendAndCheckReadsSucceedWithRate(
            {rst: rst, db: db, cmd: cmd, minRate: min, maxRate: max, burstCount: kBurstCount});
    }
}

/* Verify mirror reads behavior for various commands. */
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
    verifyMirrorReads(rst, kDbName, {find: kCollName, filter: {}, maxTimeMS: kLargeMaxTimeMS});

    jsTestLog("Verifying mirrored reads for 'count' commands");
    verifyMirrorReads(rst, kDbName, {count: kCollName, query: {}, maxTimeMS: kLargeMaxTimeMS});

    jsTestLog("Verifying mirrored reads for 'distinct' commands");
    verifyMirrorReads(rst, kDbName, {distinct: kCollName, key: "x", maxTimeMS: kLargeMaxTimeMS});

    jsTestLog("Verifying mirrored reads for 'findAndModify' commands");
    verifyMirrorReads(rst, kDbName, {
        findAndModify: kCollName,
        query: {},
        update: {'$inc': {x: 1}},
        maxTimeMS: kLargeMaxTimeMS
    });

    jsTestLog("Verifying mirrored reads for 'update' commands");
    verifyMirrorReads(rst, kDbName, {
        update: kCollName,
        updates: [{q: {_id: 1}, u: {'$inc': {x: 1}}}],
        ordered: false,
        maxTimeMS: kLargeMaxTimeMS
    });

    jsTestLog("Verifying processedAsSecondary field for 'find' commands timing out on secondaries");
    verifyProcessedAsSecondaryOnEarlyError(rst);

    jsTestLog("Verifying erroredDuringSend field for 'find' commands failing to send");
    verifyErroredDuringSend(rst);

    if (FeatureFlagUtil.isEnabled(rst.getPrimary(), "BulkWriteCommand")) {
        jsTestLog("Verifying mirrored reads for 'bulkWrite' commands");
        verifyMirrorReads(rst, "admin", {
            bulkWrite: 1,
            ops: [
                {insert: 1, document: {_id: 0, x: 0}},
                {update: 0, filter: {_id: 0}, updateMods: {'$inc': {x: 1}}, upsert: true},
                {update: 1, filter: {_id: 1}, updateMods: {'$inc': {x: 1}}, upsert: true},
            ],
            nsInfo: [{ns: kDbName + ".coll1"}, {ns: kDbName + ".coll2"}],
            ordered: false,
            maxTimeMS: kLargeMaxTimeMS
        });
    }

    // Test that the pending metric works with just the `failpoint.mirrorMaestroTracksPending` fail
    // point.
    assert.commandWorked(rst.getPrimary().adminCommand(
        {configureFailPoint: "mirrorMaestroExpectsResponse", mode: "off"}));
    let initialStatsOnPrimary = getMirroredReadsStats(rst.getPrimary());
    sendReads({
        rst: rst,
        db: kDbName,
        cmd: {find: kCollName, filter: {}, maxTimeMS: kLargeMaxTimeMS},
        burstCount: kBurstCount,
        initialStatsOnPrimary: initialStatsOnPrimary
    });
    assert.soon(() => {
        let currentStatsOnPrimary = getMirroredReadsStats(rst.getPrimary());
        const statDifferenceOnPrimary =
            getStatDifferences(initialStatsOnPrimary, currentStatsOnPrimary);
        let sent = statDifferenceOnPrimary.sent;
        let erroredDuringSend = statDifferenceOnPrimary.erroredDuringSend;
        let seen = statDifferenceOnPrimary.seen;
        // `pending` refers to the number of reads the primary has decided to mirror to
        // secondaries, but hasn't yet resolved. Unlike the other mirrored reads metrics, this
        // metric does not accumulate, and refers only to the reads currently pending.
        let pending = currentStatsOnPrimary.pending;

        jsTestLog(
            "Verifying that no mirrored reads remain pending: " +
            tojson(
                {sent: sent, erroredDuringSend: erroredDuringSend, pending: pending, seen: seen}));
        // Verify that no more reads are pending.
        return (pending == 0);
    }, "Did not resolve all pending requests within the time limit", 10000);

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

/* Verify that the distribution of mirrored reads between hosts is reasonable. */
function verifyMirroringDistribution(rst) {
    const samplingRate = 0.5;
    const gaussDeviation = .34;
    const max = samplingRate + gaussDeviation;
    const min = samplingRate - gaussDeviation;

    jsTestLog(`Running test with sampling rate = ${samplingRate}`);
    assert.commandWorked(setParameter({rst: rst, value: {samplingRate: samplingRate}}));

    let before = getMirroredReadsStats(rst.getPrimary());

    sendAndCheckReadsSucceedWithRate({
        rst: rst,
        db: kDbName,
        cmd: {find: kCollName, filter: {}},
        minRate: min,
        maxRate: max,
        burstCount: kBurstCount,
    });

    let after = getMirroredReadsStats(rst.getPrimary());

    let mean = computeMean(before, after);
    let std = computeSTD(before, after, mean);
    jsTestLog(`Mean =  ${mean}; STD = ${std}`);

    // Verify that relative standard deviation is less than 25%.
    let relativeSTD = std / mean;
    assert(relativeSTD < 0.25);
}

/* Verify a reasonable distribution of mirrored reads for various secondary counts. */
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
