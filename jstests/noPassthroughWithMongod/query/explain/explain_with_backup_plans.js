/**
 * Verify that when a backup plan is present, the backup plan is reported as a rejected plan in explain output for all verbosity levels.
 *
 */

import {getQueryPlanner, planHasStage} from "jstests/libs/query/analyze_plan.js";

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const pipeline = [{$sort: {a: 1}}, {$match: {t: 1}}];

const docs = [];
for (let i = 0; i < 100; ++i) {
    docs.push({});
}

assert.commandWorked(coll.insert(docs));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({t: 1}));

function runAndAssertExplain(verbosity) {
    const explain =
        verbosity === undefined ? coll.explain().aggregate(pipeline) : coll.explain(verbosity).aggregate(pipeline);

    const label = verbosity === undefined ? "default(queryPlanner)" : verbosity;
    jsTest.log.info("Explain (" + label + "): " + tojson(explain));

    const qp = getQueryPlanner(explain);

    assert(qp.winningPlan, "Expected a winningPlan in queryPlanner, got: " + tojson(qp));
    assert(planHasStage(db, qp.winningPlan, "SORT"), "Expected winning plan to have a SORT stage, got: " + tojson(qp));
    assert(qp.rejectedPlans, "Expected rejectedPlans in queryPlanner, got: " + tojson(qp));
    assert.eq(
        qp.rejectedPlans.length,
        1,
        "Expected one rejected plan (backup plan) in " + label + " explain, got: " + tojson(qp),
    );
    assert(
        !planHasStage(db, qp.rejectedPlans[0], "SORT"),
        "Expected rejected plan to NOT have a SORT stage, got: " + tojson(qp),
    );
}

runAndAssertExplain(); // default == "queryPlanner"
runAndAssertExplain("queryPlanner");
runAndAssertExplain("executionStats");
runAndAssertExplain("allPlansExecution");

// Now force the blocking sort to fail due to memory constraints so that the backup plan is
// actually executed, and verify that the query still returns correct results (the backup plan
// took over).
//
// The scenario requires:
//   1. A blocking sort plan that WINS multi-planning (via forced index intersection's +3 boost)
//   2. The winning plan producing 0 results during trials (sort buffers but doesn't output)
//   3. A competing non-blocking plan taken as backup
//   4. The sort exceeding memory DURING EXECUTION (not during the trial period)
//
// To achieve this:
//   - Insert enough matching docs that the sort overflows a low memory limit.
//   - Force intersection plans so the AND_SORTED+SORT plan wins despite 0 trial productivity.
//   - Use a short trial period so the sort only buffers a few docs during planning.
//   - Disable disk spilling so the sort throws QueryExceededMemoryLimitNoDiskUseAllowed.
//   - Force classic engine since backup plans are a classic MultiPlanStage feature.

coll.drop();

const N = 200;
const bulkDocs = [];
for (let i = 0; i < N; ++i) {
    bulkDocs.push({a: 1, b: 1});
}
assert.commandWorked(coll.insert(bulkDocs));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

const paramsToSet = {
    internalQueryForceIntersectionPlans: true,
    internalQueryPlannerEnableSortIndexIntersection: true,
    // Low enough to fail when sorting ~200 docs, high enough to survive the short trial period
    // (~5 docs buffered * ~200 bytes each = ~1000 bytes).
    internalQueryMaxBlockingSortMemoryUsageBytes: 5000,
    // The sort must throw instead of silently spilling to disk.
    allowDiskUseByDefault: false,
    // Short trial period: each plan gets 5 work() calls. The blocking sort only buffers ~5 docs
    // (well within the 5KB limit) and produces 0 results. The intersection plan still wins due
    // to the +3 forced-intersection score boost.
    internalQueryPlanEvaluationWorks: 5,
    // Backup plans only exist in the classic MultiPlanStage.
    internalQueryFrameworkControl: "forceClassicEngine",
};

const savedParams = {};
for (const param of Object.keys(paramsToSet)) {
    const result = assert.commandWorked(db.adminCommand({getParameter: 1, [param]: 1}));
    savedParams[param] = result[param];
}
for (const [param, value] of Object.entries(paramsToSet)) {
    assert.commandWorked(db.adminCommand({setParameter: 1, [param]: value}));
}

try {
    // The query {a: 1, b: 1} with sort {b: 1} and forced intersection generates:
    //   Plan A: AND_SORTED(IXSCAN(a), IXSCAN(b)) + FETCH + SORT(b)   [blocking, +3 boost -> wins]
    //   Plan B: IXSCAN(b:1) + FETCH(filter: a=1)                     [non-blocking -> backup]
    //   Plan C: IXSCAN(a:1) + FETCH + SORT(b)                        [blocking -> rejected]
    //

    // Step 1: Verify the backup plan exists via queryPlanner explain.
    // With queryPlanner verbosity the plan is NOT executed, so the backup switch doesn't happen.
    {
        const explain = assert.commandWorked(coll.find({a: 1, b: 1}).sort({b: 1}).explain("queryPlanner"));
        const qp = getQueryPlanner(explain);
        assert(qp.winningPlan, "Expected a winningPlan, got: " + tojson(qp));
        // The intersection+sort plan should be the winner (it has the +3 boost).
        assert(
            planHasStage(db, qp.winningPlan, "SORT"),
            "Expected winning plan to have a SORT stage, got: " + tojson(qp),
        );
        // The backup (non-blocking IXSCAN on b) should appear as a rejected plan.
        assert.eq(qp.rejectedPlans.length, 2, "Expected two rejected plan. Got: " + tojson(qp));
        assert(
            !planHasStage(db, qp.rejectedPlans[0], "SORT"),
            "Expected backup plan to not have a SORT stage, got: " + tojson(qp),
        );
    }

    // Step 2: Verify the backup plan is actually executed when the sort exceeds memory.
    {
        const result = coll.find({a: 1, b: 1}).sort({b: 1}).toArray();
        assert.eq(
            result.length,
            N,
            "Expected all " + N + " documents to be returned by the backup plan after the sort plan failed",
        );

        const explain = assert.commandWorked(coll.find({a: 1, b: 1}).sort({b: 1}).explain("allPlansExecution"));
        const qp = getQueryPlanner(explain);
        assert(qp.winningPlan, "Expected a winningPlan, got: " + tojson(qp));
        // The backup plan should be the winner after the sort plan fails during execution.
        assert(
            !planHasStage(db, qp.winningPlan, "SORT"),
            "Expected winning plan to NOT have a SORT stage, got: " + tojson(qp),
        );
        // Plans containing blocking sort should appear as rejected plans.
        assert.eq(qp.rejectedPlans.length, 2, "Expected two rejected plan. Got: " + tojson(qp));
        assert(
            planHasStage(db, qp.rejectedPlans[0], "SORT") && planHasStage(db, qp.rejectedPlans[1], "SORT"),
            "Expected rejected plans to have a SORT stage, got: " + tojson(qp),
        );
    }
} finally {
    for (const [param, value] of Object.entries(savedParams)) {
        assert.commandWorked(db.adminCommand({setParameter: 1, [param]: value}));
    }
}
