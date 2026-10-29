/**
 * Tests that join selectivity uses the larger join-key NDV of the two sides. The larger collection's
 * NDV is only used when it comes from a unique index or persisted NDV statistics.
 *
 * @tags: [
 *   featureFlagPersistentStats,
 *   requires_fcv_91,
 *   requires_sbe,
 * ]
 */
import {getAllPlanStages, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {joinOptUsed} from "jstests/libs/query/join_utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

// 'source' is how the NDV of join key "k" is obtained: "sampling", "hll" (persisted NDV
// statistics) or "unique" (a unique index).
function makeColl(db, name, source, {numDocs, ndv = numDocs}) {
    const coll = db[name];
    assert.commandWorked(
        coll.insertMany(Array.from({length: numDocs}, (_, i) => ({_id: i, k: i % ndv}))),
    );
    // Non-unique indexes provide the path arrayness info the join optimizer requires.
    assert.commandWorked(coll.createIndex({dummy: 1, k: 1}));
    if (source === "unique") {
        assert.commandWorked(coll.createIndex({k: 1}, {unique: true}));
    } else if (source === "hll") {
        assert.commandWorked(db.runCommand({analyze: name, mode: "ndv", key: "k"}));
    }
    return coll;
}

// Checks the join's NDV estimate with each collection on either side of the $lookup.
function assertNdvEstimate(small, large, expectedPkSide, expectedSource) {
    // Syntactic join order shouldn't matter, so try both orders.
    for (const [local, foreign] of [
        [small, large],
        [large, small],
    ]) {
        const explain = local.explain().aggregate([
            {
                $lookup: {
                    from: foreign.getName(),
                    localField: "k",
                    foreignField: "k",
                    as: "f",
                },
            },
            {$unwind: "$f"},
        ]);
        assert(joinOptUsed(explain), "expected join optimization", {explain});
        const joins = getAllPlanStages(getWinningPlanFromExplain(explain)).filter(
            (s) => s.ndvEstimates,
        );
        assert.eq(joins.length, 1, "expected one join", {explain});
        const [est] = joins[0].ndvEstimates;
        assert.eq(est.assumedPkSide, expectedPkSide.getFullName(), {est});
        assert.eq(est.ndvSource, expectedSource, {est});
    }
}

describe("two-sided join NDV estimation", function () {
    let conn;
    let db;

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagPersistentStats: true,
                internalQueryEnablePersistentNDVStats: true,
                internalEnableJoinOptimization: true,
                // Reports 'ndvEstimates' on join stages.
                internalQueryExplainJoinCostComponents: true,
            },
        });
        db = conn.getDB("test");
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    describe("larger side has more distinct values", function () {
        // The small collection holds a foreign key into the large one, so the larger NDV is the
        // right one, but the large collection's NDV is never sampled.
        const cases = [
            {small: "sampling", large: "sampling", pkSide: "small", ndvSource: "Sampling"},
            {small: "sampling", large: "hll", pkSide: "large", ndvSource: "HyperLogLog"},
            {small: "sampling", large: "unique", pkSide: "large", ndvSource: "Metadata"},
            {small: "hll", large: "sampling", pkSide: "small", ndvSource: "HyperLogLog"},
            {small: "hll", large: "hll", pkSide: "large", ndvSource: "HyperLogLog"},
            {small: "hll", large: "unique", pkSide: "large", ndvSource: "Metadata"},
            {small: "unique", large: "sampling", pkSide: "small", ndvSource: "Metadata"},
            {small: "unique", large: "hll", pkSide: "large", ndvSource: "HyperLogLog"},
            {small: "unique", large: "unique", pkSide: "large", ndvSource: "Metadata"},
        ];
        for (const {small, large, pkSide, ndvSource} of cases) {
            it(`small side ${small}, large side ${large}`, function () {
                const colls = {
                    small: makeColl(db, `small_${small}_${large}`, small, {numDocs: 4}),
                    large: makeColl(db, `large_${small}_${large}`, large, {numDocs: 500}),
                };
                assertNdvEstimate(colls.small, colls.large, colls[pkSide], ndvSource);
            });
        }
    });

    describe("smaller side has more distinct values", function () {
        it("keeps the small side's sampled NDV", function () {
            const small = makeColl(db, "small_fewer_sampling", "sampling", {numDocs: 4});
            const large = makeColl(db, "large_fewer_sampling", "hll", {numDocs: 500, ndv: 2});
            assertNdvEstimate(small, large, small, "Sampling");
        });

        it("keeps the small side's unique index NDV", function () {
            const small = makeColl(db, "small_fewer_unique", "unique", {numDocs: 4});
            const large = makeColl(db, "large_fewer_unique", "hll", {numDocs: 500, ndv: 2});
            assertNdvEstimate(small, large, small, "Metadata");
        });
    });
});
