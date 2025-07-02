/**
 * Verify that mirroredReads happen in response to setParameters.mirroredReads
 *
 * @tags: [
 *   requires_replication,
 *   requires_fcv_62
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {MirrorReadsHelpers} from "jstests/libs/mirror_reads_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kDbName = "mirrored_reads_test";
const kCollName = "coll";
// We use an arbitrarily large maxTimeMS to avoid timing out when processing the mirrored read
// on slower builds. We want to give the secondary.mirroredReads.processedAsSecondary metric as
// much time as possible to increment.
const kLargeMaxTimeMS = 100000000;

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
    rst.initiate();

    jsTestLog(`Attempting invalid mirrorReads parameters`);
    assert.commandFailed(
        MirrorReadsHelpers.setParameter({nodeToReadFrom: rst.getPrimary(), value: 0.5}));
    assert.commandFailed(
        MirrorReadsHelpers.setParameter({nodeToReadFrom: rst.getPrimary(), value: "half"}));
    assert.commandFailed(MirrorReadsHelpers.setParameter(
        {nodeToReadFrom: rst.getPrimary(), value: {samplingRate: -1.0}}));
    assert.commandFailed(MirrorReadsHelpers.setParameter(
        {nodeToReadFrom: rst.getPrimary(), value: {samplingRate: 1.01}}));
    assert.commandFailed(MirrorReadsHelpers.setParameter(
        {nodeToReadFrom: rst.getPrimary(), value: {somplingRate: 1.0}}));

    // Put in a datum
    {
        let ret =
            rst.getPrimary().getDB(kDbName).runCommand({insert: kCollName, documents: [{x: 1}]});
        assert.commandWorked(ret);
    }

    jsTestLog("Verifying mirrored reads for 'find' commands");
    MirrorReadsHelpers.verifyMirrorReads(rst,
                                         MirrorReadsHelpers.kGeneralMode,
                                         kDbName,
                                         {find: kCollName, filter: {}, maxTimeMS: kLargeMaxTimeMS});

    jsTestLog("Verifying mirrored reads for 'count' commands");
    MirrorReadsHelpers.verifyMirrorReads(rst,
                                         MirrorReadsHelpers.kGeneralMode,
                                         kDbName,
                                         {count: kCollName, query: {}, maxTimeMS: kLargeMaxTimeMS});

    jsTestLog("Verifying mirrored reads for 'distinct' commands");
    MirrorReadsHelpers.verifyMirrorReads(
        rst,
        MirrorReadsHelpers.kGeneralMode,
        kDbName,
        {distinct: kCollName, key: "x", maxTimeMS: kLargeMaxTimeMS});

    jsTestLog("Verifying mirrored reads for 'findAndModify' commands");
    MirrorReadsHelpers.verifyMirrorReads(rst, MirrorReadsHelpers.kGeneralMode, kDbName, {
        findAndModify: kCollName,
        query: {},
        update: {'$inc': {x: 1}},
        maxTimeMS: kLargeMaxTimeMS
    });

    jsTestLog("Verifying mirrored reads for 'update' commands");
    MirrorReadsHelpers.verifyMirrorReads(rst, MirrorReadsHelpers.kGeneralMode, kDbName, {
        update: kCollName,
        updates: [{q: {_id: 1}, u: {'$inc': {x: 1}}}],
        ordered: false,
        maxTimeMS: kLargeMaxTimeMS
    });

    jsTestLog("Verifying processedAsSecondary field for 'find' commands timing out on secondaries");
    MirrorReadsHelpers.verifyProcessedAsSecondaryOnEarlyError(
        rst, MirrorReadsHelpers.kGeneralMode, kDbName, kCollName);

    jsTestLog("Verifying erroredDuringSend field for 'find' commands failing to send");
    MirrorReadsHelpers.verifyErroredDuringSend(
        rst, MirrorReadsHelpers.kGeneralMode, kDbName, kCollName);

    if (FeatureFlagUtil.isEnabled(rst.getPrimary(), "BulkWriteCommand")) {
        jsTestLog("Verifying mirrored reads for 'bulkWrite' commands");
        MirrorReadsHelpers.verifyMirrorReads(rst, MirrorReadsHelpers.kGeneralMode, "admin", {
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
    let initialStatsOnPrimary = MirrorReadsHelpers.getMirroredReadsStats(rst.getPrimary(), kDbName);
    MirrorReadsHelpers.sendReads({
        nodeToReadFrom: rst.getPrimary(),
        db: kDbName,
        cmd: {find: kCollName, filter: {}, maxTimeMS: kLargeMaxTimeMS},
        burstCount: MirrorReadsHelpers.kBurstCount,
        initialStatsOnReadingNode: initialStatsOnPrimary
    });
    assert.soon(() => {
        let currentStatsOnPrimary =
            MirrorReadsHelpers.getMirroredReadsStats(rst.getPrimary(), kDbName);
        const statDifferenceOnPrimary =
            MirrorReadsHelpers.getStatDifferences(initialStatsOnPrimary, currentStatsOnPrimary);
        let sent = statDifferenceOnPrimary.sent;
        let erroredDuringSend = statDifferenceOnPrimary.erroredDuringSend;
        let seen = statDifferenceOnPrimary.seen;
        // `pending` refers to the number of reads the primary should mirror, but hasn't yet
        // scheduled. Unlike the other mirrored reads metrics, this metric does not accumulate, and
        // refers only to the reads currently pending.
        let pending = currentStatsOnPrimary.pending;
        // `scheduled` refers to the number of reads the primary has scheduled mirrors for, but
        // hasn't yet resolved. Unlike the other mirrored reads metrics, this metric does not
        // accumulate, and refers only to the reads currently scheduled.
        let scheduled = currentStatsOnPrimary.scheduled;

        jsTestLog("Verifying that no mirrored reads remain unresolved: " + tojson({
                      sent: sent,
                      erroredDuringSend: erroredDuringSend,
                      pending: pending,
                      scheduled: scheduled,
                      seen: seen
                  }));
        // Verify that no more reads are unresolved.
        return (pending == 0 && scheduled == 0);
    }, "Did not resolve all requests within the time limit", 10000);

    rst.stopSet();
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
        rst.initiate();

        jsTestLog(`Verifying mirroring distribution for ${secondaries} secondaries`);
        MirrorReadsHelpers.verifyMirroringDistribution(
            rst, MirrorReadsHelpers.kGeneralMode, kDbName, kCollName);
        rst.stopSet();
    }
}
