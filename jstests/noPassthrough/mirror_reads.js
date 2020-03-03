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

function sendAndCheckReads({rst, cmd, minRate, maxRate}) {
    let startMirroredReads =
        rst.getPrimary().getDB(kDbName).serverStatus({mirroredReads: 1}).mirroredReads;

    jsTestLog(`Sending ${kBurstCount} request burst of ${tojson(cmd)} to primary`);

    // Blast out a set of trivial reads
    for (var i = 0; i < kBurstCount; ++i) {
        rst.getPrimary().getDB(kDbName).runCommand(cmd);
    }

    jsTestLog(`Verifying ${tojson(cmd)} was mirrored`);

    // Verify that the reads have been observed on the primary
    {
        let currentMirroredReads =
            rst.getPrimary().getDB(kDbName).serverStatus({mirroredReads: 1}).mirroredReads;
        assert.eq(startMirroredReads.seen + kBurstCount, currentMirroredReads.seen);
    }

    // Verify that the reads mirrored to the secondaries have responded
    assert.soon(() => {
        let currentMirroredReads =
            rst.getPrimary().getDB(kDbName).serverStatus({mirroredReads: 1}).mirroredReads;

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
})();
