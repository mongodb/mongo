/**
 * Verify basic targeted mirrored reads behavior
 *
 * @tags: [
 *   requires_replication,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

function setParameter({rst, value}) {
    return rst.getPrimary().adminCommand({setParameter: 1, mirrorReads: value});
}

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

const rst = new ReplSetTest({
    nodes: 3,
});
rst.startSet();
rst.initiate();

jsTestLog(`Attempting invalid targetedMirrorReads options`);
assert.commandFailed(setParameter({rst: rst, value: {targetedMirroring: 1}}));
assert.commandFailed(setParameter({rst: rst, value: {targetedMirroring: {samplingRate: ""}}}));
assert.commandFailed(setParameter({rst: rst, value: {targetedMirroring: {maxTimeMS: ""}}}));
assert.commandFailed(setParameter({rst: rst, value: {targetedMirroring: {tag: ""}}}));
assert.commandFailed(
    setParameter({rst: rst, value: {samplingRate: 1.0, targetedMirroring: {maxTimeMS: ""}}}));

jsTestLog(`Correctly set targetedMirrorReads options`);
let targetedOptions = {samplingRate: 0.5, maxTimeMS: 3000, tag: {"foo": "bar"}};
assert.commandWorked(setParameter({rst: rst, value: {targetedMirroring: targetedOptions}}));
let parameterOptions = rst.getPrimary().adminCommand({getParameter: 1, mirrorReads: 1}).mirrorReads;
verifyParameterOptions(
    parameterOptions, {samplingRate: 0.01, maxTimeMS: 1000} /* default options */, targetedOptions);

targetedOptions = {
    samplingRate: 1.0,
    maxTimeMS: 10000
};
assert.commandWorked(
    setParameter({rst: rst, value: {samplingRate: 0.5, targetedMirroring: targetedOptions}}));
parameterOptions = rst.getPrimary().adminCommand({getParameter: 1, mirrorReads: 1}).mirrorReads;
verifyParameterOptions(parameterOptions,
                       {samplingRate: 0.5, maxTimeMS: 1000 /* default maxTimeMS */},
                       {samplingRate: 1.0, maxTimeMS: 10000, tag: {} /* default tag */});

rst.stopSet();
