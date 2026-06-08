/**
 * Tests that excessively long input pipeline names don't OOM the server.
 *
 * @tags: [
 *   featureFlagRankFusionFull,
 *   featureFlagSearchHybridScoringFull,
 *   requires_fcv_82,
 * ]
 */

import {describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();
assert.commandWorked(coll.insertOne({_id: 1}));

const kWeightsMismatchCode = 9967500;

const rankFusionPipeline = [{$sort: {_id: 1}}];
const scoreFusionPipeline = [{$score: {score: {$const: 1.0}}}];

function buildWeightsCommand(pipelineNames, weightNames, stageName) {
    const inputPipeline = stageName === "$rankFusion" ? rankFusionPipeline : scoreFusionPipeline;
    const pipelines = {};
    for (const name of pipelineNames) {
        pipelines[name] = inputPipeline;
    }
    const weights = {};
    for (const name of weightNames) {
        weights[name] = 1.0;
    }
    const stageSpec =
        stageName === "$rankFusion"
            ? {input: {pipelines}, combination: {weights}}
            : {input: {pipelines, normalization: "none"}, combination: {weights}};
    return {aggregate: collName, pipeline: [{[stageName]: stageSpec}], cursor: {}};
}

const kStages = ["$rankFusion", "$scoreFusion"];

describe("$rankFusion/$scoreFusion OOM guard for typo suggestion", function () {
    it("handles large payload (two 60K-char pipelines, one mismatched weight) without OOM", function () {
        const longA = "A".repeat(60000);
        const longC = "C".repeat(60000);
        const longB = "B".repeat(60000);
        for (const stageName of kStages) {
            assert.commandFailedWithCode(
                db.runCommand(buildWeightsCommand([longA, longC], [longB], stageName)),
                kWeightsMismatchCode,
                {stageName},
            );
        }
    });

    it("server remains reachable after handling the oversized payload", function () {
        const longA = "A".repeat(60000);
        const longC = "C".repeat(60000);
        const longB = "B".repeat(60000);
        for (const stageName of kStages) {
            assert.commandFailedWithCode(
                db.runCommand(buildWeightsCommand([longA, longC], [longB], stageName)),
                kWeightsMismatchCode,
                {stageName},
            );
        }
        assert.commandWorked(db.runCommand({ping: 1}));
    });

    it("falls back to listing all valid pipelines when budget is exceeded", function () {
        const longA = "A".repeat(60000);
        const longC = "C".repeat(60000);
        const longB = "B".repeat(60000);
        for (const stageName of kStages) {
            const res = db.runCommand(buildWeightsCommand([longA, longC], [longB], stageName));
            assert.commandFailedWithCode(res, kWeightsMismatchCode, {stageName});
            // Both valid pipeline names should appear in the error message as unranked suggestions.
            assert.includes(res.errmsg, longA, "expected first pipeline name in error message");
            assert.includes(res.errmsg, longC, "expected second pipeline name in error message");
        }
    });

    it("short names produce ranked typo suggestions", function () {
        // With names well under the budget, the Levenshtein-ranked path should still work and
        // suggest the closest match rather than listing everything.
        for (const stageName of kStages) {
            const res = db.runCommand(buildWeightsCommand(["kite", "mite"], ["kit"], stageName));
            assert.commandFailedWithCode(res, kWeightsMismatchCode, {stageName});
            // "kit" is 1 edit from "kite" and 2 from "mite", so only "kite" should be suggested.
            assert.includes(res.errmsg, "suggested: 'kite'", {stageName, errmsg: res.errmsg});
            assert(!res.errmsg.includes("mite"), "expected 'mite' not to appear in suggestion for 'kit'", {
                stageName,
                errmsg: res.errmsg,
            });
        }
    });
});
