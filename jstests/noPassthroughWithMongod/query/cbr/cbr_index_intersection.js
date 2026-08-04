/**
 * Test that cost-based ranking costs index intersection plans (AND_SORTED / AND_HASH) and uses
 * those costs to choose between intersection and single-index plans.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    ce,
    getPlans,
    getPlansWithStage,
    numKeys,
    Stage,
} from "jstests/noPassthroughWithMongod/query/cbr/cbr_expect_helpers.js";
import {getCost} from "jstests/libs/query/cbr_utils.js";
import {
    getQueryPlanner,
    getWinningPlanFromExplain,
    planHasStage,
} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

if (checkSbeFullyEnabled(db)) {
    jsTest.log.info(`Skipping ${jsTestName()} as SBE is not fully enabled.`);
    quit();
}

const coll = db[jsTestName()];

function setParams(params) {
    assert.commandWorked(db.adminCommand(Object.assign({setParameter: 1}, params)));
}

describe("CBR with index intersection", function () {
    let originalParams;

    before(function () {
        originalParams = assert.commandWorked(
            db.adminCommand({
                getParameter: 1,
                internalQueryPlanRanker: 1,
                internalQueryCBRCEMode: 1,
                internalQueryPlannerEnableHashIntersection: 1,
                internalQueryPlannerEnableSortIndexIntersection: 1,
                internalQueryForceIntersectionPlans: 1,
            }),
        );
        delete originalParams.ok;
        setParams({
            internalQueryPlanRanker: "costBased",
            internalQueryCBRCEMode: "exactCE",
            internalQueryPlannerEnableHashIntersection: true,
            internalQueryPlannerEnableSortIndexIntersection: true,
        });

        assert(coll.drop());
        // 1000 documents:
        // - 'a' is 1 for the first 500 documents.
        // - 'b' is 1 for documents 490..989, so {a: 1, b: 1} matches exactly 10 documents while
        //   each predicate alone matches 500.
        // - 'c' is unique, 'd' is constant, so {c: X, d: 1} is highly selective on 'c' alone.
        const docs = [];
        for (let i = 0; i < 1000; i++) {
            docs.push({
                _id: i,
                a: i < 500 ? 1 : 0,
                b: i >= 490 && i < 990 ? 1 : 0,
                c: i,
                d: 1,
            });
        }
        assert.commandWorked(coll.insertMany(docs));
        assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}, {c: 1}, {d: 1}]));
    });

    after(function () {
        setParams(originalParams);
        assert(coll.drop());
    });

    it("estimates exact cardinality and key counts on an AND_SORTED plan", function () {
        const explain = coll.find({a: 1, b: 1}).explain();
        const intersectionPlans = getPlansWithStage(explain, "AND_SORTED");
        assert.gt(intersectionPlans.length, 0, "no AND_SORTED plan was enumerated", {explain});
        // Each index scan reads its full 500-key posting list; the intersection returns the 10
        // documents matching both predicates.
        intersectionPlans[0]
            .expect("AND_SORTED", ce.eq(10))
            .expect("IXSCAN", ce.eq(500), numKeys.eq(500));
    });

    it("estimates exact cardinality on an AND_HASH plan", function () {
        // Non-point bounds make the enumerator produce AND_HASH rather than AND_SORTED.
        const explain = coll.find({a: {$gte: 1}, b: {$gte: 1}}).explain();
        const intersectionPlans = getPlansWithStage(explain, "AND_HASH");
        assert.gt(intersectionPlans.length, 0, "no AND_HASH plan was enumerated", {explain});
        intersectionPlans[0].expect("AND_HASH", ce.eq(10));
    });

    it("chooses the cheapest plan, and the winner's cost is minimal among all candidates", function () {
        const explain = coll.find({a: 1, b: 1}).explain();
        const planner = getQueryPlanner(explain);
        const winningCost = getCost(planner.winningPlan);
        for (const rejected of planner.rejectedPlans) {
            assert.lte(winningCost, getCost(rejected), "winning plan is not the cheapest", {
                explain,
            });
        }
    });

    it("does not choose an intersection when one predicate is highly selective", function () {
        // {c: 5} matches a single document; intersecting with the 1000-key index on 'd' can only
        // add cost.
        const explain = coll.find({c: 5, d: 1}).explain();
        const winningPlan = getWinningPlanFromExplain(explain);
        assert(!planHasStage(db, winningPlan, "AND_SORTED"), "expected no AND_SORTED in winner", {
            explain,
        });
        assert(!planHasStage(db, winningPlan, "AND_HASH"), "expected no AND_HASH in winner", {
            explain,
        });
        new Stage(winningPlan).expect("IXSCAN", numKeys.eq(1));
    });

    it("costs intersection plans under heuristic CE without falling back", function () {
        setParams({internalQueryCBRCEMode: "heuristicCE"});
        try {
            const explain = coll.find({a: 1, b: 1}).explain();
            const intersectionPlans = getPlansWithStage(explain, "AND_SORTED");
            assert.gt(intersectionPlans.length, 0, "no AND_SORTED plan was enumerated", {explain});
            const stage = intersectionPlans[0].getStage("AND_SORTED");
            // Heuristic CE won't be exact, but the intersection node must have been estimated
            // with a sane cardinality.
            assert(stage.ce !== undefined, "AND_SORTED stage has no cardinality estimate", {
                explain,
            });
            assert.between(0, stage.ce, coll.count(), "AND_SORTED CE out of range", true);
            // All plans, intersection included, must carry a cost estimate (no CBR fallback).
            for (const plan of getPlans(explain)) {
                getCost(plan.explain);
            }
        } finally {
            setParams({internalQueryCBRCEMode: "exactCE"});
        }
    });
});
