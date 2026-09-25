/**
 * Tests the $unionWith sub-pipeline's explain output at the V3 verbosities.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getQueryPlanner, isV3QueryPlanner} from "jstests/libs/query/analyze_plan.js";

const collName = jsTestName();
const coll = db[collName];

const command = {
    aggregate: collName,
    pipeline: [
        {$unionWith: {coll: collName, pipeline: [{$match: {sub: 2}}]}},
        {$match: {pushed: {$gte: 0}}},
    ],
    cursor: {},
};

const kV3Verbosities = ["plannerChoice", "plannerStats", "execStats"];

// The only V3 verbosity that executes, and so the only one reporting execution statistics.
const kExecutingVerbosity = "execStats";

// The $unionWith sub-pipeline is serialized as a bare stage array. Returns it so it can be wrapped
// as {stages: ...} for the shared explain accessors, which expect an explain root.
function subPipelineOf(explain) {
    const unionWithStage = explain.stages.find((stage) => stage.hasOwnProperty("$unionWith"));
    assert(unionWithStage, "missing $unionWith stage", {explain});
    const subPipeline = unionWithStage.$unionWith.pipeline;
    assert(Array.isArray(subPipeline), "expected a serialized sub-pipeline array", {explain});
    return subPipeline;
}

describe("$unionWith sub-pipeline explain at the V3 verbosities", function () {
    let savedFrameworkControl;

    before(function () {
        savedFrameworkControl = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
        ).internalQueryFrameworkControl;
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
        );

        coll.drop();
        assert.commandWorked(
            coll.insert([
                {sub: 1, pushed: 1},
                {sub: 2, pushed: 2},
                {sub: 2, pushed: 3},
            ]),
        );
        assert.commandWorked(coll.createIndex({sub: 1}));
    });

    after(function () {
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryFrameworkControl: savedFrameworkControl,
            }),
        );
        coll.drop();
    });

    for (const verbosity of kV3Verbosities) {
        it(`reconstructs the pushed-down stage at ${verbosity}`, function () {
            const explain = assert.commandWorked(db.runCommand({explain: command, verbosity}));
            const queryPlanner = getQueryPlanner({stages: subPipelineOf(explain)});

            // Assert that both the sub-pipeline and pushed-down predicates are present.
            assert(queryPlanner.hasOwnProperty("parsedQuery"), "missing parsedQuery", {explain});
            const predicateFields = Object.keys(
                queryPlanner.parsedQuery.$and
                    ? Object.assign({}, ...queryPlanner.parsedQuery.$and)
                    : queryPlanner.parsedQuery,
            );
            assert.contains("sub", predicateFields, "sub-pipeline predicate lost", {explain});
            assert.contains(
                "pushed",
                predicateFields,
                "pushed-down predicate lost from the reconstructed sub-pipeline",
                {explain},
            );
        });

        it(`reports sub-pipeline sections per policy at ${verbosity}`, function () {
            const explain = assert.commandWorked(db.runCommand({explain: command, verbosity}));
            const subPipeline = subPipelineOf(explain);

            // A V3 request reaches the sub-pipeline too: its $cursor renders the V3 'plans' array
            // rather than a mapped legacy winningPlan/rejectedPlans pair.
            assert.eq(
                isV3QueryPlanner(getQueryPlanner({stages: subPipeline})),
                true,
                "unexpected sub-pipeline queryPlanner shape",
                {explain},
            );

            // Execution statistics appear in the sub-pipeline exactly when the verbosity executes.
            const cursorStage = subPipeline[0].$cursor;
            assert(cursorStage, "expected a leading $cursor in the sub-pipeline", {explain});
            assert.eq(
                cursorStage.hasOwnProperty("executionStats"),
                verbosity === kExecutingVerbosity,
                "unexpected sub-pipeline executionStats presence",
                {explain},
            );
        });
    }
});
