/**
 * Verify basic targeted mirrored reads behavior.
 *
 * @tags: [
 *   requires_replication,
 *   requires_fcv_82,
 * ]
 */

import {MirrorReadsHelpers} from "jstests/libs/mirror_reads_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kDbName = "targeted_mirror_reads_test";
const kCollName = "coll";
// We use an arbitrarily large maxTimeMS to avoid timing out when processing the mirrored read
// on slower builds. We want to give the secondary.mirroredReads.processedAsSecondary metric as
// much time as possible to increment.
const kLargeMaxTimeMS = 100000000;

function verifyParameterOptions(
    parameterOptions, expectedStandardMirroringOptions, expectedTargetedMirroringOptions) {
    assert.hasFields(parameterOptions, ["samplingRate", "maxTimeMS", "targetedMirroring"]);

    assert.eq(parameterOptions.samplingRate, expectedStandardMirroringOptions.samplingRate);
    assert.eq(parameterOptions.maxTimeMS, expectedStandardMirroringOptions.maxTimeMS);

    assert.hasFields(parameterOptions.targetedMirroring, ["samplingRate", "maxTimeMS", "tag"]);
    assert.eq(parameterOptions.targetedMirroring.samplingRate,
              expectedTargetedMirroringOptions.samplingRate);
    assert.eq(parameterOptions.targetedMirroring.maxTimeMS,
              expectedTargetedMirroringOptions.maxTimeMS);
    assert.eq(parameterOptions.targetedMirroring.tag, expectedTargetedMirroringOptions.tag);
}

// Test that setting mirrorReads on a standalone is valid.
const conn = MongoRunner.runMongod();
assert.commandWorked(conn.getDB("admin").runCommand({
    setParameter: 1,
    mirrorReads: {
        "samplingRate": 0.05,
        "maxTimeMS": 5234,
        "targetedMirroring": {"samplingRate": 0.07, "maxTimeMS": 7234, "tag": {"hello": "world"}}
    }
}));
MongoRunner.stopMongod(conn);

const rst = new ReplSetTest({
    nodes: 4,
    nodeOptions: {
        setParameter: {
            "failpoint.mirrorMaestroExpectsResponse": tojson({mode: "alwaysOn"}),
            "failpoint.mirrorMaestroTracksPending": tojson({mode: "alwaysOn"}),
            "mirrorReads": tojson({targetedMirroring: {samplingRate: 1.0, tag: {"a": "1"}}})
        }
    }
});
rst.startSet();
rst.initiate();

jsTestLog(`Attempting invalid targetedMirrorReads options`);
assert.commandFailed(MirrorReadsHelpers.setParameter(
    {nodeToReadFrom: rst.getPrimary(), value: {targetedMirroring: 1}}));
assert.commandFailed(MirrorReadsHelpers.setParameter(
    {nodeToReadFrom: rst.getPrimary(), value: {targetedMirroring: {samplingRate: ""}}}));
assert.commandFailed(MirrorReadsHelpers.setParameter(
    {nodeToReadFrom: rst.getPrimary(), value: {targetedMirroring: {maxTimeMS: ""}}}));
assert.commandFailed(MirrorReadsHelpers.setParameter(
    {nodeToReadFrom: rst.getPrimary(), value: {targetedMirroring: {tag: ""}}}));
assert.commandFailed(MirrorReadsHelpers.setParameter({
    nodeToReadFrom: rst.getPrimary(),
    value: {samplingRate: 1.0, targetedMirroring: {maxTimeMS: ""}}
}));

jsTestLog(`Correctly set targetedMirrorReads options`);
let targetedOptions = {samplingRate: 0.5, maxTimeMS: 3000, tag: {"foo": "bar"}};
assert.commandWorked(MirrorReadsHelpers.setParameter(
    {nodeToReadFrom: rst.getPrimary(), value: {targetedMirroring: targetedOptions}}));
let parameterOptions = rst.getPrimary().adminCommand({getParameter: 1, mirrorReads: 1}).mirrorReads;
verifyParameterOptions(
    parameterOptions, {samplingRate: 0.01, maxTimeMS: 1000} /* default options */, targetedOptions);

targetedOptions = {
    samplingRate: 1.0,
    maxTimeMS: 10000
};
assert.commandWorked(MirrorReadsHelpers.setParameter({
    nodeToReadFrom: rst.getPrimary(),
    value: {samplingRate: 0.5, targetedMirroring: targetedOptions}
}));
parameterOptions = rst.getPrimary().adminCommand({getParameter: 1, mirrorReads: 1}).mirrorReads;
verifyParameterOptions(parameterOptions,
                       {samplingRate: 0.5, maxTimeMS: 1000 /* default maxTimeMS */},
                       {samplingRate: 1.0, maxTimeMS: 10000, tag: {} /* default tag */});

// Now check targeted mirroring behavior

// First, set the sampling rate to 0 so none of the initial reads are mirrored. This will make the
// assertions made later more predictable.
rst.nodes.forEach(function(node) {
    assert.commandWorked(MirrorReadsHelpers.setParameter(
        {nodeToReadFrom: node, value: {targetedMirroring: {samplingRate: 0.0}}}));
});

jsTestLog(`Doing a reconfig to set tags on secondaries`);
let tagOne = {"tag": "one"};
let tagTwo = {"tag": "two"};

let primary = rst.getPrimary();
let rsConfig = primary.getDB("local").system.replset.findOne();
rsConfig.members.forEach(function(member) {
    if (member.host != primary.host) {
        if (member._id % 2 == 0) {
            member.tags = tagOne;
        } else {
            member.tags = tagTwo;
        }
    }
});
rsConfig.version++;
assert.commandWorked(primary.adminCommand({replSetReconfig: rsConfig}));
rst.awaitSecondaryNodes();

// Assert the secondaries have the tag, but the primary does not.
rsConfig = primary.getDB("local").system.replset.findOne();
rsConfig.members.forEach(function(member) {
    if (member.host != primary.host) {
        if (member._id % 2 == 0) {
            member.tags = tagOne;
        } else {
            member.tags = tagTwo;
        }
    } else {
        assert.eq(member.tags, {});
    }
});

// Put in a datum
{
    let ret = rst.getPrimary().getDB(kDbName).runCommand({insert: kCollName, documents: [{x: 1}]});
    assert.commandWorked(ret);
}

{
    jsTestLog(`Testing targeted mirrored reads with tag: ` + tojson(tagOne));

    jsTestLog("Verifying mirrored reads for 'find' commands");
    MirrorReadsHelpers.verifyMirrorReads(rst,
                                         MirrorReadsHelpers.kTargetedMode,
                                         kDbName,
                                         {find: kCollName, filter: {}, maxTimeMS: kLargeMaxTimeMS},
                                         tagOne);

    jsTestLog("Verifying mirrored reads for 'count' commands");
    MirrorReadsHelpers.verifyMirrorReads(rst,
                                         MirrorReadsHelpers.kTargetedMode,
                                         kDbName,
                                         {count: kCollName, query: {}, maxTimeMS: kLargeMaxTimeMS},
                                         tagOne);

    jsTestLog("Verifying mirrored reads for 'distinct' commands");
    MirrorReadsHelpers.verifyMirrorReads(
        rst,
        MirrorReadsHelpers.kTargetedMode,
        kDbName,
        {distinct: kCollName, key: "x", maxTimeMS: kLargeMaxTimeMS},
        tagOne);
}

{
    jsTestLog(`Now testing targeted mirrored reads with tag: ` + tojson(tagTwo));

    jsTestLog("Verifying mirrored reads for 'find' commands");
    MirrorReadsHelpers.verifyMirrorReads(rst,
                                         MirrorReadsHelpers.kTargetedMode,
                                         kDbName,
                                         {find: kCollName, filter: {}, maxTimeMS: kLargeMaxTimeMS},
                                         tagTwo);

    jsTestLog("Verifying mirrored reads for 'count' commands");
    MirrorReadsHelpers.verifyMirrorReads(rst,
                                         MirrorReadsHelpers.kTargetedMode,
                                         kDbName,
                                         {count: kCollName, query: {}, maxTimeMS: kLargeMaxTimeMS},
                                         tagTwo);

    jsTestLog("Verifying mirrored reads for 'distinct' commands");
    MirrorReadsHelpers.verifyMirrorReads(
        rst,
        MirrorReadsHelpers.kTargetedMode,
        kDbName,
        {distinct: kCollName, key: "x", maxTimeMS: kLargeMaxTimeMS},
        tagTwo);
}

{
    jsTestLog(`Doing reconfig to change the set of nodes tagged with targetedMirroredReads tag`);
    let samplingRate = 1.0;
    let nodeToReadFrom = rst.getSecondaries()[0];
    let secondariesWithTag =
        MirrorReadsHelpers.getSecondariesWithTargetedTag(nodeToReadFrom, tagTwo);
    // Execute setParameter before the reconfig so that we can be sure the reconfig is the thing
    // that triggers the hosts list to be updated.
    assert.commandWorked(MirrorReadsHelpers.setParameter({
        nodeToReadFrom: nodeToReadFrom,
        value: {targetedMirroring: {samplingRate: samplingRate, tag: tagTwo}}
    }));

    // Call this directly to ensure the setParameter isn't the trigger for updating the hosts, but
    // that the hosts are correctly updated after a reconfig
    jsTestLog("Verifying mirrored reads for 'find' commands");
    MirrorReadsHelpers.sendAndCheckReadsSucceedWithRate({
        rst: rst,
        nodeToReadFrom: rst.getSecondaries()[0],
        secondariesWithTag: secondariesWithTag,
        mirrorMode: MirrorReadsHelpers.kTargetedMode,
        db: kDbName,
        cmd: {find: kCollName, filter: {}, maxTimeMS: kLargeMaxTimeMS},
        minRate: samplingRate,
        maxRate: samplingRate,
        burstCount: MirrorReadsHelpers.kBurstCount
    });
}

// Reset param to avoid mirroring before next test case
assert.commandWorked(MirrorReadsHelpers.setParameter(
    {nodeToReadFrom: rst.getSecondaries()[0], value: {targetedMirroring: {samplingRate: 0.0}}}));

jsTestLog("Verifying erroredDuringSend field for 'find' commands failing to send");
MirrorReadsHelpers.verifyErroredDuringSend(
    rst, MirrorReadsHelpers.kTargetedMode, kDbName, kCollName, tagTwo);

rst.stopSet();
