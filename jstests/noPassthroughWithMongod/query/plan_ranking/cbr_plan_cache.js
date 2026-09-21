/**
 * Establish the multiplanning replanning behavior that we would like to preserve.
 * Verify that CBR is able to choose a plan that gets cached.
 * Verify that CBR is able to be invoked during replanning.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */

import {
    getPlanCacheShapeHashFromObject,
    getCachedPlanForQuery,
    assertPlanHasIxScanStage,
} from "jstests/libs/query/analyze_plan.js";
import {getPlanRankerConfig, setPlanRankerConfig} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullyEnabled, isDeferredGetExecutorEnabled} from "jstests/libs/query/sbe_util.js";

// CBR cannot rank plans for SBE-targeted queries when the deferred engine choice is disabled:
// PlanRanker::rankPlans() forces such queries onto the multiplanner, so the CBR fallback
// strategies below are never exercised and their expectations would not hold.
const canRunCbrPhases = !checkSbeFullyEnabled(db) || isDeferredGetExecutorEnabled(db);

const collName = jsTestName();
const coll = db[collName];
coll.drop();

const docs = [];
const kNumDocs = 15000;

for (let i = 0; i < kNumDocs; i++) {
    docs.push({a: i, b: i});
}
docs.push({a: 7000 + 1, b: 7000 + 1, c: 1});
docs.push({a: 8000 + 1, b: 8000 + 1, c: 1});
coll.insertMany(docs);

// Enumerate 2 possible plans:
//  1) IXSCAN over 'a'
//  2) IXSCAN with 'b'
coll.createIndexes([{a: 1}, {b: 1}]);

// The predicate on 'b' is more selective.
const bIndexQuery = {a: {$gte: 1}, b: {$gte: 14500}, c: 1};
// Predicate on 'a' is more selective.
const aIndexQuery = {a: {$gte: 100}, b: {$gte: 1}, c: 1};

const aIndexQueryExpectedCQLog =
    "ns=test.cbr_plan_cacheTree: $and\n    c $eq 1.0\n    a $gte 100.0\n    b $gte 1.0\nSort: {}\nProj: {}\n";
// We expect this to be increased only on 'aIndexQuery', if CBR is enabled.
let expectedNumCBRChoseCachedPlanLogs = 0;

function runCommand(command, isMultiplanning) {
    command();
    if (!isMultiplanning) {
        expectedNumCBRChoseCachedPlanLogs += 1;
    }
}

function runInitialCacheTest(isMultiplanning) {
    jsTest.log.info("Running runInitialCacheTest", {isMultiplanning});
    coll.getPlanCache().clear();

    // aIndexQuery will hit the CBR fallback mechanism.
    runCommand(() => coll.find(aIndexQuery).toArray(), isMultiplanning);

    // The plan cache should now hold an inactive entry.
    let entry = getCachedPlanForQuery(db, coll, aIndexQuery);

    let planCacheShapeHash = getPlanCacheShapeHashFromObject(entry);
    assert.eq(entry.isActive, false);
    assertPlanHasIxScanStage(false /* isSbePlanCacheEnabled */, entry, "a_1", planCacheShapeHash);

    if (isMultiplanning) {
        assert.eq(entry.creationExecStats.length, 2); // One for each candidate plan.
        assert.eq(entry.candidatePlanScores.length, 2); // One for each candidate plan.
    } else {
        // TODO SERVER-116684: Change these assertions.
        assert.eq(entry.creationExecStats.length, 1);
        assert.eq(entry.candidatePlanScores.length, 1);
    }

    // Running the query again activates the cache entry.
    runCommand(() => coll.find(aIndexQuery).toArray(), isMultiplanning);

    entry = getCachedPlanForQuery(db, coll, aIndexQuery);

    assert.eq(entry.isActive, true);
    assertPlanHasIxScanStage(false /* isSbePlanCacheEnabled */, entry, "a_1", planCacheShapeHash);

    if (isMultiplanning) {
        assert.eq(entry.creationExecStats.length, 2); // One for each candidate plan.
        assert.eq(entry.candidatePlanScores.length, 2); // One for each candidate plan.
    } else {
        // TODO SERVER-116684: Change these assertions.
        assert.eq(entry.creationExecStats.length, 1);
        assert.eq(entry.candidatePlanScores.length, 1);
    }
}

function runReplanningTest(isMultiplanning) {
    jsTest.log.info("Running runReplanningTest", {isMultiplanning});
    coll.getPlanCache().clear();

    coll.find(bIndexQuery).toArray();
    let currWorks = 501;

    // The plan cache should now hold an inactive entry.
    let entry = getCachedPlanForQuery(db, coll, bIndexQuery);
    let planCacheShapeHash = getPlanCacheShapeHashFromObject(entry);
    let entryWorks = entry.works;
    const entryPlanCacheKey = entry.planCacheKey;
    assert.eq(entry.isActive, false);
    assert.eq(entryWorks, currWorks);
    assertPlanHasIxScanStage(false /* isSbePlanCacheEnabled */, entry, "b_1", planCacheShapeHash);

    // Running the query again activates the cache entry.
    coll.find(bIndexQuery).toArray();
    entry = getCachedPlanForQuery(db, coll, bIndexQuery);
    assert.eq(entry.isActive, true);
    assert.eq(entry.works, entryWorks);
    assert.eq(entry.planCacheKey, entryPlanCacheKey);
    assertPlanHasIxScanStage(false /* isSbePlanCacheEnabled */, entry, "b_1", planCacheShapeHash);

    const strategy = assert.commandWorked(
        db.adminCommand({
            getParameter: 1,
            internalQueryMixedPlanRankingStrategy: 1,
        }),
    )["internalQueryMixedPlanRankingStrategy"];
    // We happen to use CBR to pick the winning plan for 'bIndexQuery' with EstimateRankingEffort,
    // and MP with NoMultiplanningResults.
    if (isMultiplanning || strategy === "NoMultiplanningResults") {
        assert.eq(entry.creationExecStats.length, 2); // One for each candidate plan.
        assert.eq(entry.candidatePlanScores.length, 2); // One for each candidate plan.
    } else if (strategy === "EstimateRankingEffort") {
        // TODO SERVER-116684: Change these assertions.
        assert.eq(entry.creationExecStats.length, 1);
        assert.eq(entry.candidatePlanScores.length, 1);
    } else {
        assert(false, `Unhandled strategy: ${strategy}`);
    }

    // This query will trigger replanning since the number of works is vastly higher than the cached plan.
    // Because of this, the new plan will not be active at first.
    runCommand(() => coll.find(aIndexQuery).toArray(), isMultiplanning);
    entry = getCachedPlanForQuery(db, coll, aIndexQuery);
    assert.eq(entry.isActive, false);
    assert.eq(entry.planCacheKey, entryPlanCacheKey);
    assertPlanHasIxScanStage(false /* isSbePlanCacheEnabled */, entry, "a_1", planCacheShapeHash);

    if (isMultiplanning) {
        assert.eq(entry.creationExecStats.length, 2); // One for each candidate plan.
        assert.eq(entry.candidatePlanScores.length, 2); // One for each candidate plan.
    } else {
        // TODO SERVER-116684: Change these assertions.
        assert.eq(entry.creationExecStats.length, 1);
        assert.eq(entry.candidatePlanScores.length, 1);
    }

    const growthCoefficient = assert.commandWorked(
        db.adminCommand({getParameter: 1, internalQueryCacheWorksGrowthCoefficient: 1}),
    ).internalQueryCacheWorksGrowthCoefficient;

    // Activate the plan. For this example, we need to run the query 5 times in order for the inactive cache
    // entry's number of works to grow larger (via the 'growthCoefficient') than the winning
    // plan's number of works, at which point the plan will be active.
    for (let i = 0; i < 5; i++) {
        // Cache entry should be inactive throughout this loop.
        assert(!entry.isActive);

        // When the number of works of the plan in the cache is less than the number of works taken to choose
        // the winning plan for this run of the query, the number of works in the cache entry grows by this coefficient.
        currWorks *= growthCoefficient;
        assert.eq(entry.works, currWorks);

        runCommand(() => coll.find(aIndexQuery).toArray(), isMultiplanning);

        entry = getCachedPlanForQuery(db, coll, aIndexQuery);
    }

    // The cache entry is now active, with a number of works that is accurate to the aIndexQuery's
    // number of works. The 10000 comes from the limit of the number of works the multiplanner can
    // do before it chooses a winning plan. In this case, since there are only 2 documents that
    // match the query, by the time we get to 10000 works, we will not have filled a batch.
    assert(entry.isActive);
    assert.eq(entry.planCacheKey, entryPlanCacheKey);
    assert.eq(entry.works, 10000);
}

const prevPlanRankerConfig = getPlanRankerConfig(db);

const prevQueryKnobs = assert.commandWorked(
    db.adminCommand({
        getParameter: 1,
        internalQuerySamplingBySequentialScan: 1,
    }),
);

const prevSequentialSamplingScan = prevQueryKnobs.internalQuerySamplingBySequentialScan;
let originalLogLevel = assert.commandWorked(db.setLogLevel(2, "query")).was.query.verbosity;

// Use deterministic sampling to avoid plan instability.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: true}),
);

try {
    // 1: Run with only MultiPlanning.
    db.adminCommand({setParameter: 1, featureFlagCostBasedRanker: false});
    runInitialCacheTest(true /* isMultiplanning */);
    runReplanningTest(true /* isMultiplanning */);

    // 2: Run with CBR fallback strategies.
    if (canRunCbrPhases) {
        db.adminCommand({
            setParameter: 1,
            featureFlagCostBasedRanker: true,
            internalQueryPlanRanker: "mixed",
            internalQueryCBRCEMode: "samplingCE",
        });

        const cbrFallbackStrategies = ["NoMultiplanningResults", "EstimateRankingEffort"];

        for (const cbrFallbackStrategy of cbrFallbackStrategies) {
            jsTest.log.info("Running runInitialCacheTest", {cbrFallbackStrategy});
            db.adminCommand({
                setParameter: 1,
                internalQueryMixedPlanRankingStrategy: cbrFallbackStrategy,
            });
            runInitialCacheTest(false /* isMultiplanning */);

            jsTest.log.info("Running replanningTest", {cbrFallbackStrategy});
            runReplanningTest(false /* isMultiplanning */);
        }
    } else {
        jsTest.log.info(
            "Skipping CBR fallback strategies: SBE is enabled without deferred engine choice",
        );
    }

    // TODO SERVER-116989: Run tests under the non-release CBR configurations (e.g. sampling).
} finally {
    setPlanRankerConfig(db, prevPlanRankerConfig);

    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQuerySamplingBySequentialScan: prevSequentialSamplingScan,
        }),
    );

    assert.commandWorked(db.setLogLevel(originalLogLevel, "query"));
}
