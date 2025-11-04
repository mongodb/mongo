/**
 * Tests that ExtractFieldPathsStage appears underneath $group.
 *
 * @tags: [
 *    assumes_against_mongod_not_mongos,
 *    # Explain command does not support read concerns other than local.
 *    assumes_read_concern_local,
 *    assumes_read_concern_unchanged,
 *    assumes_unsharded_collection,
 *    # We modify the value of a query knob. setParameter is not persistent.
 *    does_not_support_stepdowns,
 *    # Explain for the aggregate command cannot run within a multi-document transaction.
 *    does_not_support_transactions,
 *    requires_fcv_83,
 * ]
 */
import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {getSbePlanStages} from "jstests/libs/query/sbe_explain_helpers.js";

const originalFrameworkControl = db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1});
const originalFeatureFlagExtract = db.adminCommand({getParameter: 1, featureFlagExtractFieldPathsSbeStage: 1});

try {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));
    assert.commandWorked(db.adminCommand({setParameter: 1, featureFlagExtractFieldPathsSbeStage: true}));
    db.c.deleteMany({});
    db.c.insert({_id: 0, a: {b: 1, c: 1}});
    const pipelines = [{$group: {_id: {ab: "$a.b", ac: "$a.c"}, idSum: {$sum: "$_id"}}}];
    for (let pipeline of pipelines) {
        jsTest.log({"pipeline": pipeline});
        const explain = db.c.explain("executionStats").aggregate(pipeline);
        const extractStages = getSbePlanStages(explain, "extract_field_paths");
        assert.eq(extractStages.length, 1, "Should have one extract_field_paths stage");
        assert.eq(extractStages[0]["stage"], "extract_field_paths", "Stage name should match");
    }
} finally {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryFrameworkControl: originalFrameworkControl.internalQueryFrameworkControl,
        }),
    );
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            featureFlagExtractFieldPathsSbeStage: originalFeatureFlagExtract.featureFlagExtractFieldPathsSbeStage,
        }),
    );
}
