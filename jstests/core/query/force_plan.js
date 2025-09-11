/**
 * Test using the forcedPlanSolutionHash parameter to the find and
 * explain commands.
 * @tags: [
 *    assumes_unsharded_collection,
 *    assumes_against_mongod_not_mongos,
 *    # We modify the value of a query knob. setParameter is not persistent.
 *    does_not_support_stepdowns,
 *    # Explain for the aggregate command cannot run within a multi-document transaction.
 *    does_not_support_transactions,
 *    # Explain command does not support read concerns other than local.
 *    assumes_read_concern_local,
 *    assumes_read_concern_unchanged,
 *    # This test asserts on candidate set size which makes it incompatible.
 *    assumes_no_implicit_index_creation,
 *    # Cannot enable query knob under mongoq simulation.
 *    simulate_mongoq_incompatible,
 *    requires_fcv_83,
 * ]
 */
import {getAllPlans} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

assert.commandWorked(coll.insertMany([{a: 4}, {a: 5}, {a: 6}]));

/*
 * Run all plans in the provided candidate list one by one using it's solution hash.
 */
function explainAndRunAllPlans(command) {
    const explainRes = assert.commandWorked(db.runCommand({explain: command, verbosity: "queryPlanner"}));
    const candidates = getAllPlans(explainRes);

    for (const candidate of candidates) {
        assert(candidate.hasOwnProperty("solutionHashUnstable"), tojson(candidate));
        assert.neq(candidate.solutionHashUnstable, 0, tojson(candidate));

        const forcedPlanExplain = assert.commandWorked(
            db.runCommand({
                explain: {...command, forcedPlanSolutionHash: candidate.solutionHashUnstable},
                verbosity: "queryPlanner",
            }),
        );
        const plans = getAllPlans(forcedPlanExplain);
        // Should only be one plan when we force the plan.
        assert.eq(1, plans.length);
        // It should have an equal solution hash.
        assert.eq(plans[0].solutionHashUnstable, candidate.solutionHashUnstable, tojson(plans[0]));
        // Sanity check that the root stage is the same type.
        assert.eq(plans[0].stage, candidate.stage, tojson(plans[0]));
    }

    return candidates;
}

function testQueryWithMultipleEnumeratedPlans() {
    const allPlans = explainAndRunAllPlans({find: jsTestName(), filter: {a: {$gt: 0}}, sort: {b: 1}});

    // We should see a plan with a bounds on index a:1 and a plan with a sort on b:1.
    assert.eq(2, allPlans.length);
}

function testNonMatchingHash() {
    // Command should fail if the forced solution hash doesn't match any candidate plan.
    assert.commandFailedWithCode(
        db.runCommand({
            explain: {
                find: jsTestName(),
                filter: {a: {$gt: 0}},
                sort: {b: 1},
                // Presumably no solution hash will ever match this.
                forcedPlanSolutionHash: NumberLong(-1),
            },
            verbosity: "queryPlanner",
        }),
        ErrorCodes.NoQueryExecutionPlans,
    );
}

function testSingleSolution() {
    const allPlans = explainAndRunAllPlans({find: jsTestName(), filter: {a: {$gt: 1}}});
    assert.eq(1, allPlans.length);
}

function testSubplanning() {
    // Command should fail on a rooted $or query.
    assert.commandFailedWithCode(
        db.runCommand({
            explain: {
                find: jsTestName(),
                filter: {$or: [{a: {$gt: 0}}, {a: {$lt: 10}}]},
                sort: {b: 1},
                forcedPlanSolutionHash: NumberLong(0),
            },
            verbosity: "queryPlanner",
        }),
        ErrorCodes.IllegalOperation,
    );
}

let originalParamValue;

// Wrap the test in setting the internalQueryAllowForcedPlanByHash which also controls whether the
// solution hash is included in the explain output.
try {
    originalParamValue = db.adminCommand({getParameter: 1, internalQueryAllowForcedPlanByHash: 1});
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryAllowForcedPlanByHash: true}));

    testQueryWithMultipleEnumeratedPlans();
    testNonMatchingHash();
    testSingleSolution();
    testSubplanning();
} finally {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryAllowForcedPlanByHash: originalParamValue.internalQueryAllowForcedPlanByHash,
        }),
    );
}

// find() command should fail if the query knob is not set.
assert.commandFailedWithCode(
    db.runCommand({find: jsTestName(), filter: {}, forcedPlanSolutionHash: 123}),
    ErrorCodes.FailedToParse,
);

// aggregate() command should fail if the query knob is not set.
assert.commandFailedWithCode(
    db.runCommand({aggregate: jsTestName(), pipeline: [], forcedPlanSolutionHash: 123}),
    ErrorCodes.FailedToParse,
);
