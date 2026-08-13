/**
 * Tests that "planRanker" is attributed to the operation's own plan. The strategy is sourced from
 * the PlanExplainer via PlanSummaryStats, so a sub-pipeline executor built during execution must not
 * overwrite the outer query's value, and a getMore must report the strategy that chose the cursor's
 * plan rather than a fresh default.
 *
 * Also covers rooted $or, where the sub-planner plans each branch separately and so only learns
 * the strategy while planning. A branch is ranked only when it has competing candidates, and when
 * subplanning is abandoned the whole-query plan's own strategy is what gets reported.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_profiling,
 * ]
 */
import {before, describe, it} from "jstests/libs/mochalite.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {ChosenRanker, getEngine, getRejectedPlans} from "jstests/libs/query/analyze_plan.js";
import {setPlanRankerConfig} from "jstests/libs/query/cbr_utils.js";

const coll = db[jsTestName()];
const foreignColl = db[jsTestName() + "_foreign"];

// Runs 'fn' with profiling on and returns the profiler entry matching 'filter'.
function profile(filter, fn) {
    try {
        assert.commandWorked(db.setProfilingLevel(2, {slowms: 0}));
        fn();
    } finally {
        assert.commandWorked(db.setProfilingLevel(0));
    }
    return getLatestProfilerEntry(db, filter);
}

describe("planRanker attribution", function () {
    before(function () {
        // Two competing indexes plus a predicate on each field guarantee more than one candidate
        // plan, so the multi-planner genuinely ranks.
        coll.drop();
        foreignColl.drop();
        const docs = [];
        for (let i = 0; i < 1000; i++) {
            // 'h1' and 'h2' are left unindexed here; the mixed-ranker case below gives them the
            // hashed indexes it needs.
            docs.push({a: i % 5, b: i, h1: i % 13, h2: i % 17});
        }
        assert.commandWorked(coll.insert(docs));
        assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));

        // No indexes on the foreign collection, so its sub-pipeline has a single solution.
        assert.commandWorked(foreignColl.insert([{b: 6}, {b: 7}]));

        setPlanRankerConfig(db, {featureFlagCostBasedRanker: false});
    });

    it("a single-solution $lookup sub-pipeline does not overwrite the outer query's strategy", function () {
        coll.getPlanCache().clear();
        const marker = "planRankerAttributionLookup";
        const entry = profile({op: "command", "command.comment": marker}, () => {
            assert.eq(
                coll
                    .aggregate(
                        [
                            {$match: {a: 1, b: 6}},
                            {
                                $lookup: {
                                    from: foreignColl.getName(),
                                    localField: "b",
                                    foreignField: "b",
                                    as: "joined",
                                },
                            },
                        ],
                        {comment: marker},
                    )
                    .itcount(),
                1,
            );
        });

        assert.eq(
            entry.planRanker,
            ChosenRanker.kMultiPlanning,
            "the outer query multi-planned, so the $lookup sub-pipeline must not report singlePlan",
            {entry},
        );
    });

    it("a getMore reports the strategy that chose the cursor's plan", function () {
        coll.getPlanCache().clear();
        const marker = "planRankerAttributionGetMore";

        // A batchSize below the match count forces at least one getMore. OpDebug is fresh for that
        // operation, so the value can only come from the cursor's own PlanExplainer.
        const entry = profile({op: "getmore", "originatingCommand.comment": marker}, () => {
            assert.eq(
                coll
                    .find({a: 1, b: {$lt: 500}})
                    .comment(marker)
                    .batchSize(2)
                    .itcount(),
                100,
            );
        });

        assert.eq(
            entry.planRanker,
            ChosenRanker.kMultiPlanning,
            "getMore should report the multi-planner that chose the cursor's plan",
            {entry},
        );
    });

    // Subplanner with the multi-planner ranker.
    // A rooted $or where each branch has a single indexed plan reports singlePlan.
    it("a rooted $or whose every branch has one candidate plan reports singlePlan", function () {
        coll.getPlanCache().clear();

        // One predicate per branch, each on a different indexed field: one solution per branch.
        const marker = "planRankerAttributionRootedOrSinglePlan";
        const entry = profile({op: "query", "command.comment": marker}, () => {
            coll.find({$or: [{a: 1}, {b: 900}]})
                .comment(marker)
                .itcount();
        });

        assert.eq(
            entry.planRanker,
            ChosenRanker.kSinglePlan,
            "no branch had competing plan candidates",
            {entry},
        );
    });

    it("a rooted $or with a multi-candidate branch reports multi-planner", function () {
        coll.getPlanCache().clear();

        // The first branch has a predicate on both indexed fields, so it has two candidate plans
        // and the sub-planner ranks it with the multi-planner.
        const marker = "planRankerAttributionRootedOrMultiPlan";
        const entry = profile({op: "query", "command.comment": marker}, () => {
            coll.find({$or: [{a: 1, b: {$lt: 500}}, {b: 900}]})
                .comment(marker)
                .itcount();
        });

        assert.eq(
            entry.planRanker,
            ChosenRanker.kMultiPlanning,
            "multi-planner for a branch reports the strategy for the whole operation",
            {entry},
        );
    });

    it("a rooted $or reports the plan cache once every branch is cached", function () {
        const prevDisablePlanCache = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryDisablePlanCache: 1}),
        ).internalQueryDisablePlanCache;
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryDisablePlanCache: false}),
        );

        try {
            coll.getPlanCache().clear();

            // Both branches have competing candidates, so both are multi-planned and both get a
            // cache entry on the first executions.
            const query = {
                $or: [
                    {a: 1, b: {$lt: 500}},
                    {a: 2, b: {$gt: 800}},
                ],
            };
            const numBranches = 2;

            // A branch is only tagged from the cache once its entry is active, which takes repeated
            // executions. Every branch must be cached, since one ranked branch would make the
            // operation report the multi-planner instead.
            assert.soon(() => {
                coll.find(query).itcount();
                return (
                    coll
                        .getPlanCache()
                        .list()
                        .filter((cacheEntry) => cacheEntry.isActive).length === numBranches
                );
            }, "plan cache entries for all $or branches did not become active");

            const marker = "planRankerAttributionRootedOrCached";
            const entry = profile({op: "query", "command.comment": marker}, () => {
                coll.find(query).comment(marker).itcount();
            });

            assert.eq(
                entry.planRanker,
                ChosenRanker.kCachedPlan,
                "no branch was ranked, so the strategy is the plan cache, not the multi-planner",
                {entry},
            );
        } finally {
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    internalQueryDisablePlanCache: prevDisablePlanCache,
                }),
            );
            coll.getPlanCache().clear();
        }
    });

    it("a rooted $or that abandons branch planning reports the whole-query ranker", function () {
        // The second branch predicates on an unindexed field, so branch planning fails and
        // falls back to whole query planning. It uses multiplanning to choose among 2 plans.
        assert.commandWorked(coll.createIndex({b: 1, a: 1}));
        try {
            coll.getPlanCache().clear();

            const marker = "planRankerAttributionRootedOrWholeQuery";
            const query = {$or: [{a: 1}, {unindexed: 5}]};
            const sort = {b: 1};
            const entry = profile({op: "query", "command.comment": marker}, () => {
                coll.find(query).sort(sort).comment(marker).itcount();
            });

            // The reported strategy must describe the whole-query plan, not the discarded
            // per-branch work.
            assert.eq(
                entry.planRanker,
                ChosenRanker.kMultiPlanning,
                "the whole-query fallback multi-planned",
                {entry},
            );

            // Rejected plans confirm this really was whole-query multi-planning: a composite plan
            // built by the sub-planner has none. Only the classic engine reports them here -- under
            // SBE, explain plans the query separately from execution and surfaces just the winner,
            // so the list is empty even though the operation above genuinely multi-planned.
            const explain = coll.find(query).sort(sort).explain();
            if (getEngine(explain) === "classic") {
                assert.gt(
                    getRejectedPlans(explain).length,
                    0,
                    "expected whole-query multi-planning, which leaves rejected plans",
                    {explain},
                );
            }
        } finally {
            assert.commandWorked(coll.dropIndex({b: 1, a: 1}));
            coll.getPlanCache().clear();
        }
    });

    // Subplanner with cost-based ranker.
    it("a rooted $or whose branches CBR ranks reports costBased", function () {
        try {
            // With the cost-based ranker, the sub-planner costs each branch's
            // candidates.
            setPlanRankerConfig(db, {
                internalQueryPlanRanker: "costBased",
                internalQueryCBRCEMode: "samplingCE",
            });
            coll.getPlanCache().clear();

            const marker = "planRankerAttributionRootedOrCbr";
            const entry = profile({op: "query", "command.comment": marker}, () => {
                coll.find({
                    $or: [
                        {a: 1, b: {$lt: 500}},
                        {a: 2, b: {$gt: 800}},
                    ],
                })
                    .comment(marker)
                    .itcount();
            });

            assert.eq(
                entry.planRanker,
                ChosenRanker.kCostBased,
                "CBR chose each branch's winner, so it is not the multi-planner that ranked",
                {entry},
            );
        } finally {
            // Restore the multi-planner for anything that runs after this case.
            setPlanRankerConfig(db, {featureFlagCostBasedRanker: false});
            coll.getPlanCache().clear();
        }
    });

    it("a rooted $or with one candidate per branch reports singlePlan under CBR", function () {
        try {
            setPlanRankerConfig(db, {
                internalQueryPlanRanker: "costBased",
                internalQueryCBRCEMode: "samplingCE",
            });
            coll.getPlanCache().clear();

            // Every branch has a sole candidate, Must report singlePlan.
            const marker = "planRankerAttributionRootedOrCbrSinglePlan";
            const entry = profile({op: "query", "command.comment": marker}, () => {
                coll.find({$or: [{a: 1}, {b: 900}]})
                    .comment(marker)
                    .itcount();
            });

            assert.eq(
                entry.planRanker,
                ChosenRanker.kSinglePlan,
                "a sole candidate per branch is not ranked, even with CBR as the plan ranker",
                {entry},
            );
        } finally {
            setPlanRankerConfig(db, {featureFlagCostBasedRanker: false});
            coll.getPlanCache().clear();
        }
    });

    it("a rooted $or ranked by both CBR and the multi-planner reports the multi-planner", function () {
        // CBR can only estimate plain btree index scans, so a branch whose candidates are all
        // hashed index scans falls through to the multi-planner while the other branch is
        // cost-based ranked. Two hashed indexes give that branch competing candidates, which is
        // what makes the multi-planner rank it rather than it being a lone plan.
        assert.commandWorked(coll.createIndexes([{h1: "hashed"}, {h2: "hashed"}]));
        try {
            setPlanRankerConfig(db, {
                internalQueryPlanRanker: "costBased",
                internalQueryCBRCEMode: "samplingCE",
            });
            coll.getPlanCache().clear();

            const marker = "planRankerAttributionRootedOrMixedRankers";
            const entry = profile({op: "query", "command.comment": marker}, () => {
                coll.find({
                    $or: [
                        {a: 1, b: {$lt: 500}},
                        {h1: 3, h2: 4},
                    ],
                })
                    .comment(marker)
                    .itcount();
            });

            // Branches used different strategy, so the operation reports the
            // highest-precedence one, which is the multi-planner.
            assert.eq(
                entry.planRanker,
                ChosenRanker.kMultiPlanning,
                "a multi-planned branch outranks a cost-based ranked one",
                {entry},
            );
        } finally {
            setPlanRankerConfig(db, {featureFlagCostBasedRanker: false});
            assert.commandWorked(coll.dropIndex({h1: "hashed"}));
            assert.commandWorked(coll.dropIndex({h2: "hashed"}));
            coll.getPlanCache().clear();
        }
    });
});
