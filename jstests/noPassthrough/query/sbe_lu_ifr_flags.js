/**
 * Verifies that the IFR flags controlling $lookup-$unwind (LU) pushdown to SBE work correctly
 * end-to-end across all join strategies and local-side data access plan combinations. For each
 * randomized permutation of the 7 flags, runs a set of queries and asserts that the engine
 * selection (SBE vs Classic) matches the expected outcome.
 *
 * Requires featureFlagSbeEqLookupUnwind and featureFlagGetExecutorDeferredEngineChoice to be on
 * since plan-based engine selection only fires when both are active. The test enables both flags
 * itself via MongoRunner.runMongod setParameter, so no suite-level flag is needed.
 *
 * @tags: [
 *  # This test expects join optimization to be off.
 *  incompatible_with_join_optimization
 * ]
 */

import {
    getEngine,
    getPlanStage,
    getPlanStages,
    getQueryPlanner,
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";
import {after, describe, it} from "jstests/libs/mochalite.js";
import {checkSbeCompletelyDisabled, checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagSbeEqLookupUnwind: true,
        featureFlagGetExecutorDeferredEngineChoice: true,
    },
});
assert.neq(conn, null, "mongod failed to start up");
const db = conn.getDB(jsTestName());
if (checkSbeCompletelyDisabled(db) || checkSbeFullyEnabled(db)) {
    jsTest.log.info("Skipping: IFR flags only gate LU in trySbeRestricted mode");
    MongoRunner.stopMongod(conn);
    quit();
}

// Collections:
// localNoIdx: no index on join field 'a' (uses COLLSCAN outer access)
// localWithIdx: index on join field 'a' (uses IXSCAN+FETCH outer access)
// localOrIdx: indexes on both 'a' and 'b' (uses FETCH+OR+IXSCAN outer access)
// foreignNoIdx: no index on join field 'b' (uses NLJ or HJ with allowDiskUse)
// foreignWithIdx: index on join field 'b' (uses INLJ)
// foreignDinlj: index with incompatible collation on 'b' (uses DINLJ, no disk use)

const localNoIdx = db.localNoIdx;
const localWithIdx = db.localWithIdx;
const localOrIdx = db.localOrIdx;
const foreignNoIdx = db.foreignNoIdx;
const foreignWithIdx = db.foreignWithIdx;
const foreignDinlj = db.foreignDinlj;

function setupCollections() {
    localNoIdx.drop();
    for (let i = 0; i < 5; i++) assert.commandWorked(localNoIdx.insert({_id: i, a: i}));

    localWithIdx.drop();
    for (let i = 0; i < 5; i++) assert.commandWorked(localWithIdx.insert({_id: i, a: i}));
    assert.commandWorked(localWithIdx.createIndex({a: 1}));

    // Two indexed fields so $or on a and b triggers a FETCH+OR+IXSCAN plan.
    localOrIdx.drop();
    for (let i = 0; i < 5; i++) assert.commandWorked(localOrIdx.insert({_id: i, a: i, b: i + 1}));
    assert.commandWorked(localOrIdx.createIndex({a: 1}));
    assert.commandWorked(localOrIdx.createIndex({b: 1}));

    foreignNoIdx.drop();
    for (let i = 0; i < 5; i++) assert.commandWorked(foreignNoIdx.insert({_id: i, b: i}));

    foreignWithIdx.drop();
    for (let i = 0; i < 5; i++) assert.commandWorked(foreignWithIdx.insert({_id: i, b: i}));
    assert.commandWorked(foreignWithIdx.createIndex({b: 1}));

    // String values so collation matters for DINLJ.
    // An index with a case-insensitive collation on a collection with simple (default) collation
    // makes the index collation-incompatible with simple-collation queries, triggering DINLJ when
    // allowDiskUse is false (preventing the HJ fallback).
    foreignDinlj.drop();
    for (let i = 0; i < 5; i++) assert.commandWorked(foreignDinlj.insert({_id: i, b: "hello" + i}));
    assert.commandWorked(
        foreignDinlj.createIndex({b: 1}, {collation: {locale: "en", strength: 2}}),
    );
}

setupCollections();

// Flag name constants for the 7 IFR flags.
const ACCESS_COLLSCAN = "featureFlagSbeEqLookupUnwindLocalCollscan";
const ACCESS_IXSCAN = "featureFlagSbeEqLookupUnwindLocalIxscanFetch";
const ACCESS_COMPLEX = "featureFlagSbeEqLookupUnwindLocalComplexDataAccessPlans";
const STRATEGY_HJ = "featureFlagSbeEqLookupUnwindHashJoin";
const STRATEGY_INLJ = "featureFlagSbeEqLookupUnwindIndexedLoopJoin";
const STRATEGY_NLJ = "featureFlagSbeEqLookupUnwindNestedLoopJoin";
const STRATEGY_DINLJ = "featureFlagSbeEqLookupUnwindDynamicIndexedLoopJoin";

const ALL_FLAGS = [
    ACCESS_COLLSCAN,
    ACCESS_IXSCAN,
    ACCESS_COMPLEX,
    STRATEGY_HJ,
    STRATEGY_INLJ,
    STRATEGY_NLJ,
    STRATEGY_DINLJ,
];

function setFlags(flagValues) {
    assert.commandWorked(db.adminCommand({setParameter: 1, ...flagValues}));
}

function enableAllFlags() {
    const params = {};
    for (const flag of ALL_FLAGS) {
        params[flag] = true;
    }
    assert.commandWorked(db.adminCommand({setParameter: 1, ...params}));
}

// Build a $lookup-$unwind pipeline with localField 'a' joining to foreignField 'b'.
function makeLuPipeline(foreignCollName) {
    return [
        {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "res"}},
        {$unwind: "$res"},
    ];
}

// Build a $lookup-$unwind pipeline prefixed with a selective $match on the local join field.
// The predicate causes the planner to use IXSCAN+FETCH on the outer (local) side rather than
// COLLSCAN, which it would otherwise prefer for an unfiltered full scan even when an index exists.
function makeLuIxscanPipeline(foreignCollName) {
    return [{$match: {a: {$gt: -1}}}, ...makeLuPipeline(foreignCollName)];
}

// Build a $lookup-$unwind pipeline prefixed with an $or match on two independently indexed
// fields, producing a FETCH+OR+IXSCAN outer access plan. localField 'a' joins foreignField 'b'.
function makeLuComplexPipeline(foreignCollName) {
    return [{$match: {$or: [{a: {$lt: 3}}, {b: {$gt: 2}}]}}, ...makeLuPipeline(foreignCollName)];
}

// Access-plan verification helpers.

function verifyCollscan(explain, name) {
    const plan = getWinningPlanFromExplain(explain);
    assert.gt(getPlanStages(plan, "COLLSCAN").length, 0, `${name}: outer side should use COLLSCAN`);
}

function verifyIxscanFetch(explain, name) {
    const plan = getWinningPlanFromExplain(explain);
    const fetches = getPlanStages(plan, "FETCH");
    assert.gt(fetches.length, 0, `${name}: expected FETCH stage`);
    assert(
        fetches.some((f) => f.inputStage?.stage === "IXSCAN"),
        `${name}: expected FETCH+IXSCAN`,
        {fetchInputs: fetches.map((f) => f.inputStage?.stage)},
    );
}

function verifyComplexOrFetch(explain, name) {
    const plan = getWinningPlanFromExplain(explain);
    const or = getPlanStage(plan, "OR");
    assert(or, `${name}: expected OR stage`);
    const branches = or.inputStages ?? [];
    assert.gt(branches.length, 0, `${name}: OR has no branches`);
    for (const branch of branches) {
        assert.eq(branch.stage, "IXSCAN", `${name}: OR branch must be IXSCAN`, {branch});
    }
    const fetches = getPlanStages(plan, "FETCH");
    assert(
        fetches.some((f) => f.inputStage?.stage === "OR"),
        `${name}: expected FETCH+OR`,
        {fetchInputs: fetches.map((f) => f.inputStage?.stage)},
    );
}

// Test cases grouped by local-side access plan, then by join strategy.
//
// Strategy selection rules:
//   - INLJ: foreign has a compatible-collation index
//   - HJ: no compatible index, allowDiskUse:true
//   - NLJ: no compatible index, allowDiskUse:false
//   - DINLJ: foreign has an incompatible-collation index, allowDiskUse:false
//
// strategyFlag: flag enabling SBE execution for this join strategy
// accessFlag: flag enabling SBE execution for this local access plan
// verifyPlan: asserts the explain output shows the expected local access plan shape
const TEST_CASES = [
    // COLLSCAN local access plan.
    {
        name: "HJ + COLLSCAN",
        coll: localNoIdx,
        pipeline: makeLuPipeline("foreignNoIdx"),
        options: {allowDiskUse: true},
        verifyPlan: (e) => verifyCollscan(e, "HJ + COLLSCAN"),
        strategyFlag: STRATEGY_HJ,
        accessFlag: ACCESS_COLLSCAN,
    },
    {
        name: "INLJ + COLLSCAN",
        coll: localNoIdx,
        pipeline: makeLuPipeline("foreignWithIdx"),
        verifyPlan: (e) => verifyCollscan(e, "INLJ + COLLSCAN"),
        strategyFlag: STRATEGY_INLJ,
        accessFlag: ACCESS_COLLSCAN,
    },
    {
        name: "NLJ + COLLSCAN",
        coll: localNoIdx,
        pipeline: makeLuPipeline("foreignNoIdx"),
        options: {allowDiskUse: false},
        verifyPlan: (e) => verifyCollscan(e, "NLJ + COLLSCAN"),
        strategyFlag: STRATEGY_NLJ,
        accessFlag: ACCESS_COLLSCAN,
    },
    {
        name: "DINLJ + COLLSCAN",
        // allowDiskUse:false prevents the HJ fallback when the index collation is incompatible.
        coll: localNoIdx,
        pipeline: makeLuPipeline("foreignDinlj"),
        options: {allowDiskUse: false},
        verifyPlan: (e) => verifyCollscan(e, "DINLJ + COLLSCAN"),
        strategyFlag: STRATEGY_DINLJ,
        accessFlag: ACCESS_COLLSCAN,
    },
    // Non-existent foreign collection: planner selects kNonExistentForeignCollection, gated by NLJ flag.
    {
        name: "NLJ + NONEXISTENT_FOREIGN",
        coll: localNoIdx,
        pipeline: makeLuPipeline("nonExistentForeign"),
        verifyPlan: (e) => verifyCollscan(e, "NLJ + NONEXISTENT_FOREIGN"),
        strategyFlag: STRATEGY_NLJ,
        accessFlag: ACCESS_COLLSCAN,
    },
    // IXSCAN+FETCH local access plan (selective $match pushes predicate into index scan).
    {
        name: "HJ + IXSCAN+FETCH",
        coll: localWithIdx,
        pipeline: makeLuIxscanPipeline("foreignNoIdx"),
        options: {allowDiskUse: true},
        verifyPlan: (e) => verifyIxscanFetch(e, "HJ + IXSCAN+FETCH"),
        strategyFlag: STRATEGY_HJ,
        accessFlag: ACCESS_IXSCAN,
    },
    {
        name: "INLJ + IXSCAN+FETCH",
        coll: localWithIdx,
        pipeline: makeLuIxscanPipeline("foreignWithIdx"),
        verifyPlan: (e) => verifyIxscanFetch(e, "INLJ + IXSCAN+FETCH"),
        strategyFlag: STRATEGY_INLJ,
        accessFlag: ACCESS_IXSCAN,
    },
    {
        name: "NLJ + IXSCAN+FETCH",
        coll: localWithIdx,
        pipeline: makeLuIxscanPipeline("foreignNoIdx"),
        options: {allowDiskUse: false},
        verifyPlan: (e) => verifyIxscanFetch(e, "NLJ + IXSCAN+FETCH"),
        strategyFlag: STRATEGY_NLJ,
        accessFlag: ACCESS_IXSCAN,
    },
    {
        name: "DINLJ + IXSCAN+FETCH",
        coll: localWithIdx,
        pipeline: makeLuIxscanPipeline("foreignDinlj"),
        options: {allowDiskUse: false},
        verifyPlan: (e) => verifyIxscanFetch(e, "DINLJ + IXSCAN+FETCH"),
        strategyFlag: STRATEGY_DINLJ,
        accessFlag: ACCESS_IXSCAN,
    },
    // Complex local access plan: $or on two indexed fields produces a FETCH+OR+IXSCAN plan.
    {
        name: "HJ + COMPLEX",
        coll: localOrIdx,
        pipeline: makeLuComplexPipeline("foreignNoIdx"),
        options: {allowDiskUse: true},
        verifyPlan: (e) => verifyComplexOrFetch(e, "HJ + COMPLEX"),
        strategyFlag: STRATEGY_HJ,
        accessFlag: ACCESS_COMPLEX,
    },
    {
        name: "INLJ + COMPLEX",
        coll: localOrIdx,
        pipeline: makeLuComplexPipeline("foreignWithIdx"),
        verifyPlan: (e) => verifyComplexOrFetch(e, "INLJ + COMPLEX"),
        strategyFlag: STRATEGY_INLJ,
        accessFlag: ACCESS_COMPLEX,
    },
    {
        name: "NLJ + COMPLEX",
        coll: localOrIdx,
        pipeline: makeLuComplexPipeline("foreignNoIdx"),
        options: {allowDiskUse: false},
        verifyPlan: (e) => verifyComplexOrFetch(e, "NLJ + COMPLEX"),
        strategyFlag: STRATEGY_NLJ,
        accessFlag: ACCESS_COMPLEX,
    },
    {
        name: "DINLJ + COMPLEX",
        // allowDiskUse:false prevents the HJ fallback when the index collation is incompatible.
        coll: localOrIdx,
        pipeline: makeLuComplexPipeline("foreignDinlj"),
        options: {allowDiskUse: false},
        verifyPlan: (e) => verifyComplexOrFetch(e, "DINLJ + COMPLEX"),
        strategyFlag: STRATEGY_DINLJ,
        accessFlag: ACCESS_COMPLEX,
    },
];

// Derive run (with explain) and warmup (without explain) from the struct fields so that
// the cache-warming path and the engine-check path use identical queries.
for (const tc of TEST_CASES) {
    const opts = tc.options ?? {};
    tc.run = () => tc.coll.explain().aggregate(tc.pipeline, opts);
    tc.warmup = () => tc.coll.aggregate(tc.pipeline, opts).toArray();
}

function expectedEngine(flagValues, testCase) {
    // A flag value not present in flagValues means it is on (default: true for rollout flags).
    const strategyOn = flagValues[testCase.strategyFlag] !== false;
    const accessOn = flagValues[testCase.accessFlag] !== false;
    return strategyOn && accessOn ? "sbe" : "classic";
}

function runAndVerify(flagValues, label) {
    for (const tc of TEST_CASES) {
        const explain = tc.run();
        // Always verify the local access plan shape regardless of engine selection outcome.
        tc.verifyPlan(explain);
        const actual = getEngine(explain);
        const expected = expectedEngine(flagValues, tc);
        assert.eq(
            actual,
            expected,
            `[${label}] case "${tc.name}": expected ${expected} but got ${actual}`,
            {
                flagValues,
            },
        );
    }
}

describe("LU IFR flags - all flags on (default state)", function () {
    it("every LU test case runs in SBE", function () {
        enableAllFlags();
        runAndVerify({}, "all-on");
    });
});

describe("LU IFR flags - each flag disables exactly the expected combinations", function () {
    // Disabling a strategy flag must fall back exactly the 3 combos that use that strategy.
    // Disabling an access-plan flag must fall back exactly the 4 combos that use that access plan.
    // No other combinations should be affected.
    for (const flag of ALL_FLAGS) {
        it(`disabling ${flag} causes exactly the expected combos to fall back`, function () {
            enableAllFlags();
            try {
                setFlags({[flag]: false});
                const classicCases = [];
                for (const tc of TEST_CASES) {
                    if (getEngine(tc.run()) === "classic") classicCases.push(tc.name);
                }
                const expectedCases = TEST_CASES.filter(
                    (tc) => tc.strategyFlag === flag || tc.accessFlag === flag,
                ).map((tc) => tc.name);
                assert.sameMembers(
                    classicCases,
                    expectedCases,
                    `disabling ${flag}: expected fallback ${tojson(expectedCases)}, got ${tojson(classicCases)}`,
                );
            } finally {
                enableAllFlags();
            }
        });
    }
});

describe("LU IFR flags - access-plan flags only gate LU nodes", function () {
    // When our access-plan flags are off, $group and plain $lookup (no absorbed $unwind) must
    // still run in SBE; those nodes are unconditionally eligible and our flags must not
    // interfere with them.
    it("GROUP and plain $lookup use SBE even when all access-plan flags are disabled", function () {
        enableAllFlags();
        try {
            setFlags({[ACCESS_COLLSCAN]: false, [ACCESS_IXSCAN]: false, [ACCESS_COMPLEX]: false});

            // $group on COLLSCAN local collection runs in SBE (group always enables SBE).
            assert.eq(
                getEngine(localNoIdx.explain().aggregate([{$group: {_id: "$a"}}])),
                "sbe",
                "GROUP should use SBE regardless of access-plan flags",
            );

            // Plain $lookup (no $unwind absorbed) on COLLSCAN runs in SBE.
            assert.eq(
                getEngine(
                    localNoIdx.explain().aggregate([
                        {
                            $lookup: {
                                from: "foreignNoIdx",
                                localField: "a",
                                foreignField: "b",
                                as: "r",
                            },
                        },
                    ]),
                ),
                "sbe",
                "plain $lookup (no unwind) should use SBE regardless of access-plan flags",
            );

            // Simple projection with no GROUP or $lookup is always Classic.
            assert.eq(
                getEngine(localNoIdx.explain().aggregate([{$project: {a: 1}}])),
                "classic",
                "plain project without GROUP/$lookup stays Classic",
            );
        } finally {
            enableAllFlags();
        }
    });
});

describe("LU IFR flags - randomized permutations", function () {
    // Generate random flag combinations. Each combo is logged in the it() name so CI failures
    // are fully reproducible from the log output.
    const NUM_TRIALS = 20;
    const flagCombinations = [];

    // Always include all-off as a special case.
    const allOff = {};
    for (const f of ALL_FLAGS) allOff[f] = false;
    flagCombinations.push(allOff);

    while (flagCombinations.length < NUM_TRIALS) {
        const combo = {};
        for (const flag of ALL_FLAGS) {
            combo[flag] = Math.random() < 0.5;
        }
        flagCombinations.push(combo);
    }

    for (let i = 0; i < flagCombinations.length; i++) {
        const combo = flagCombinations[i];
        it(`permutation ${i}: ${JSON.stringify(combo)}`, function () {
            jsTest.log.info("Running LU IFR permutation", {i, combo});
            // Run with all flags on (SBE) before applying the combo so that each permutation
            // also verifies that flag changes take effect even after prior SBE runs.
            enableAllFlags();
            for (const tc of TEST_CASES) tc.warmup();
            try {
                setFlags(combo);
                runAndVerify(combo, `permutation-${i}`);
            } finally {
                enableAllFlags();
            }
        });
    }
});

// The classic plan cache stores the query plan; engine selection runs AFTER retrieval
// (buildCachedPlan calls extendSolutionAndSelectEngine). To populate the classic cache we need
// multi-planning, which requires two viable index candidates. localOrIdx has indexes on {a:1}
// and {b:1}; predicate {a:0, b:1} can use either, so multi-planning runs and writes a cache entry.
describe("LU IFR flags - classic plan cache interaction", function () {
    it("IFR engine selection fires after classic plan cache retrieval", function () {
        enableAllFlags();
        // Predicate answerable by either index on localOrIdx: triggers multi-planning.
        const pipeline = [
            {$match: {a: 0, b: 1}},
            {
                $lookup: {
                    from: foreignNoIdx.getName(),
                    localField: "a",
                    foreignField: "b",
                    as: "res",
                },
            },
            {$unwind: "$res"},
        ];
        localOrIdx.getPlanCache().clear();
        // Warm the plan cache: multi-planning selects the best plan and writes it to cache.
        // The pipeline matches the single doc {a:0, b:1} which joins to {b:0} in foreignNoIdx.
        assert.eq(localOrIdx.aggregate(pipeline).toArray().length, 1, "warmup: expected 1 result");
        assert.eq(localOrIdx.aggregate(pipeline).toArray().length, 1, "warmup: expected 1 result");
        assert.eq(localOrIdx.aggregate(pipeline).toArray().length, 1, "warmup: expected 1 result");
        assert.eq(
            localOrIdx.getPlanCache().list().length,
            1,
            "expected exactly one plan cache entry after warmup",
        );
        // Verify the cache entry is active (not just inactive/pending).
        assert(localOrIdx.getPlanCache().list()[0].isActive, "expected active plan cache entry");
        // With all flags on, the plan runs in SBE (engine selection fires after cache retrieval).
        assert.eq(getEngine(localOrIdx.explain().aggregate(pipeline)), "sbe", "expected SBE");
        // Disable the IXSCAN access flag. IFR engine selection fires after the classic cache hit
        // and selects Classic. isCached: true in the explain confirms the cache was used.
        try {
            setFlags({[ACCESS_IXSCAN]: false});
            const classicExplain = localOrIdx.explain().aggregate(pipeline);
            assert.eq(
                getEngine(classicExplain),
                "classic",
                "expected Classic after ACCESS_IXSCAN disabled, even with a cached plan",
            );
            const classicWinningPlan = getQueryPlanner(classicExplain).winningPlan;
            assert(
                classicWinningPlan.isCached,
                "expected isCached: true confirming classic plan cache was used",
                {
                    classicWinningPlan,
                },
            );
        } finally {
            enableAllFlags();
        }
    });
});

// Top-level knobs that disable LU-in-SBE entirely operate above engineSelectionForPlan
// and should override our IFR flags.
describe("LU IFR flags - top-level knobs override our flags", function () {
    it("internalQuerySlotBasedExecutionDisableLookupUnwindPushdown disables LU regardless of IFR flags", function () {
        enableAllFlags();
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQuerySlotBasedExecutionDisableLookupUnwindPushdown: true,
            }),
        );
        try {
            // All our IFR flags are on, but the top-level knob forces Classic for every LU plan.
            for (const tc of TEST_CASES) {
                const explain = tc.run();
                assert.eq(
                    getEngine(explain),
                    "classic",
                    `expected classic with DisableLookupUnwindPushdown=true, case "${tc.name}"`,
                );
            }
        } finally {
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    internalQuerySlotBasedExecutionDisableLookupUnwindPushdown: false,
                }),
            );
        }
    });

    it("internalQueryFrameworkControl=forceClassicEngine disables LU regardless of IFR flags", function () {
        enableAllFlags();
        const prevFramework = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
        ).internalQueryFrameworkControl;
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
        );
        try {
            for (const tc of TEST_CASES) {
                const explain = tc.run();
                assert.eq(
                    getEngine(explain),
                    "classic",
                    `expected classic with forceClassicEngine, case "${tc.name}"`,
                );
            }
        } finally {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalQueryFrameworkControl: prevFramework}),
            );
        }
    });
});

// Root-level after hook: runs after all describe/it blocks complete (mochalite defers execution).
after(function () {
    MongoRunner.stopMongod(conn);
});
