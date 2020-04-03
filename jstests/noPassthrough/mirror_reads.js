/**
 * Verify that mirroredReads happen in response to setParameters.mirroredReads
 *
 * @tags: [requires_replication]
 */

(function() {
"use strict";

function setParameter({rst, value}) {
    return rst.getPrimary().adminCommand({setParameter: 1, mirrorReads: value});
}

const kBurstCount = 1000;
const kDbName = "mirrored_reads_test";
const kCollName = "test";

function getMirroredReadsStats(rst) {
    return rst.getPrimary().getDB(kDbName).serverStatus({mirroredReads: 1}).mirroredReads;
}

function sendAndCheckReads({rst, cmd, minRate, maxRate}) {
    let startMirroredReads = getMirroredReadsStats(rst);

    jsTestLog(`Sending ${kBurstCount} request burst of ${tojson(cmd)} to primary`);

    for (var i = 0; i < kBurstCount; ++i) {
        rst.getPrimary().getDB(kDbName).runCommand(cmd);
    }

    jsTestLog(`Verifying ${tojson(cmd)} was mirrored`);

    // Verify that the commands have been observed on the primary
    {
        let currentMirroredReads = getMirroredReadsStats(rst);
        assert.lte(startMirroredReads.seen + kBurstCount, currentMirroredReads.seen);
    }

    // Verify that the reads mirrored to the secondaries have responded
    assert.soon(() => {
        let currentMirroredReads = getMirroredReadsStats(rst);

        let readsSeen = currentMirroredReads.seen - startMirroredReads.seen;
        let readsMirrored = currentMirroredReads.resolved - startMirroredReads.resolved;

        let numNodes = rst.getSecondaries().length;
        jsTestLog(`Seen ${readsSeen} requests; ` +
                  `verified ${readsMirrored / numNodes} requests ` +
                  `x ${numNodes} nodes`);

        let rate = readsMirrored / readsSeen / numNodes;
        return (rate >= minRate) && (rate <= maxRate);
    }, "Did not verify all requests within time limit", 10000);

    jsTestLog(`Verified ${tojson(cmd)} was mirrored`);
}

function verifyMirrorReads(rst, cmd) {
    {
        jsTestLog(`Verifying disabled read mirroring with ${tojson(cmd)}`);
        let samplingRate = 0.0;

        assert.commandWorked(setParameter({rst: rst, value: {samplingRate: samplingRate}}));
        sendAndCheckReads({rst: rst, cmd: cmd, minRate: samplingRate, maxRate: samplingRate});
    }

    {
        jsTestLog(`Verifying full read mirroring with ${tojson(cmd)}`);
        let samplingRate = 1.0;

        assert.commandWorked(setParameter({rst: rst, value: {samplingRate: samplingRate}}));
        sendAndCheckReads({rst: rst, cmd: cmd, minRate: samplingRate, maxRate: samplingRate});
    }

    {
        jsTestLog(`Verifying partial read mirroring with ${tojson(cmd)}`);
        let samplingRate = 0.5;
        let gaussDeviation = .34;
        let max = samplingRate + gaussDeviation;
        let min = samplingRate - gaussDeviation;

        assert.commandWorked(setParameter({rst: rst, value: {samplingRate: samplingRate}}));
        sendAndCheckReads({rst: rst, cmd: cmd, minRate: min, maxRate: max});
    }
}

{
    const rst = new ReplSetTest({
        nodes: 3,
        nodeOptions:
            {setParameter: {"failpoint.mirrorMaestroExpectsResponse": tojson({mode: "alwaysOn"})}}
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
    verifyMirrorReads(rst, {find: kCollName, filter: {}});

    jsTestLog("Verifying mirrored reads for 'count' commands");
    verifyMirrorReads(rst, {count: kCollName, query: {}});

    jsTestLog("Verifying mirrored reads for 'distinct' commands");
    verifyMirrorReads(rst, {distinct: kCollName, key: "x"});

    jsTestLog("Verifying mirrored reads for 'findAndModify' commands");
    verifyMirrorReads(rst, {findAndModify: kCollName, query: {}, update: {'$inc': {x: 1}}});

    jsTestLog("Verifying mirrored reads for 'update' commands");
    verifyMirrorReads(
        rst, {update: kCollName, updates: [{q: {_id: 1}, u: {'$inc': {x: 1}}}], ordered: false});

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

    let before = getMirroredReadsStats(rst);

    sendAndCheckReads({rst: rst, cmd: {find: kCollName, filter: {}}, minRate: min, maxRate: max});

    let after = getMirroredReadsStats(rst);

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
                setParameter: {"failpoint.mirrorMaestroExpectsResponse": tojson({mode: "alwaysOn"})}
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
