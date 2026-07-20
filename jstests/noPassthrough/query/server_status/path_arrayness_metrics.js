/**
 * Tests serverStatus metrics under metrics.query.pathArrayness.
 *
 * @tags: [
 *   requires_sbe,
 *   featureFlagPathArrayness,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getAggPlanStage, getAggPlanStages} from "jstests/libs/query/analyze_plan.js";

function getCounters(db) {
    return db.serverStatus().metrics.query.pathArrayness;
}

describe("pathArrayness metrics", function () {
    let conn, db, coll, other;

    before(function () {
        conn = MongoRunner.runMongod();
        assert.neq(null, conn);
        db = conn.getDB(jsTestName());
        coll = db[jsTestName()];
        coll.drop();
        // x is is higher cardinality, so the planner will always prefer IXSCANs over it.
        for (let i = 0; i < 15; i++) {
            assert.commandWorked(coll.insert({_id: i, x: i, y: i, z: i % 3, w: i % 3}));
        }
        // Indexes on 'x', 'z', and 'w', PathArrayness marks them as non-array. 'y' is intentionally unindexed.
        assert.commandWorked(coll.createIndex({x: 1}));
        assert.commandWorked(coll.createIndex({z: 1}));
        assert.commandWorked(coll.createIndex({w: 1}));

        other = db[jsTestName() + "_other"];
        other.drop();
        for (let i = 0; i < 3; i++) {
            assert.commandWorked(other.insert({_id: i, z: i, label: "l" + i}));
        }
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("increments leadingFilterSimplified for a $match + $group on an indexed non-array path", function () {
        const before = getCounters(db);
        const explainResult = coll
            .explain()
            .aggregate([{$match: {x: 1, z: 1}}, {$group: {_id: "$z"}}]);
        const after = getCounters(db);

        // Check counters.
        assert.eq(after.leadingFilter - before.leadingFilter, 1);
        assert.eq(after.leadingFilterSimplified - before.leadingFilterSimplified, 1);

        // Check that the plan has a FETCH residual filter over 'z'.
        const fetchStage = getAggPlanStage(explainResult, "FETCH");
        assert(fetchStage && fetchStage.filter && fetchStage.filter.z, fetchStage);
    });

    it("doesn't increment leadingFilterSimplified when querying with an array value", function () {
        // 'w' will now contain a document with an array value, so it loses the non-array property.
        //
        // We rely on 'w' instead of 'z' because adding an array to a field with an index marks the index permanently as multikey, even if we latter remove the inserted document.
        assert.commandWorked(coll.insert({_id: 100, x: 1, w: [1, 2]}));
        try {
            const before = getCounters(db);
            const explainResult = coll
                .explain()
                .aggregate([{$match: {x: 1, w: 1}}, {$group: {_id: "$z"}}]);
            const after = getCounters(db);

            // Check counters.
            assert.eq(after.leadingFilter - before.leadingFilter, 1);
            assert.eq(after.leadingFilterSimplified - before.leadingFilterSimplified, 0);

            // Check that the plan has a FETCH residual filter over 'w'.
            const fetchStage = getAggPlanStage(explainResult, "FETCH");
            assert(fetchStage && fetchStage.filter && fetchStage.filter.w, fetchStage);
        } finally {
            assert.commandWorked(coll.deleteOne({_id: 100}));
        }
    });

    it("doesn't increment leadingFilterSimplified for a $match + $group with a residual filter on an unindexed field", function () {
        const before = getCounters(db);
        const explainResult = coll
            .explain()
            .aggregate([{$match: {x: 1, y: 1}}, {$group: {_id: "$y"}}]);
        const after = getCounters(db);

        // Check counters.
        assert.eq(after.leadingFilter - before.leadingFilter, 1);
        assert.eq(after.leadingFilterSimplified - before.leadingFilterSimplified, 0);

        // Check that the plan has a FETCH residual filter over 'y'.
        const fetchStage = getAggPlanStage(explainResult, "FETCH");
        assert(fetchStage && fetchStage.filter && fetchStage.filter.y, fetchStage);
    });

    it("increments leadingFilter by 1 (not per-predicate) for a $match + $group with mixed paths", function () {
        const before = getCounters(db);
        const explainResult = coll
            .explain()
            .aggregate([{$match: {x: 1, z: 1, y: 1}}, {$group: {_id: "$z"}}]);
        const after = getCounters(db);

        // Check counters.
        assert.eq(after.leadingFilter - before.leadingFilter, 1);
        assert.eq(after.leadingFilterSimplified - before.leadingFilterSimplified, 1);

        // Check that the FETCH residual filter contains both 'z' (indexed, simplified) and 'y' (unindexed).
        const fetchStage = getAggPlanStage(explainResult, "FETCH");
        assert(fetchStage && fetchStage.filter && fetchStage.filter.$and, fetchStage);
        const hasZ = fetchStage.filter.$and.some((f) => f.hasOwnProperty("z"));
        const hasY = fetchStage.filter.$and.some((f) => f.hasOwnProperty("y"));
        assert(hasZ && hasY, fetchStage.filter.$and);
    });

    it("increments once per query across consecutive $match + $group queries", function () {
        const before = getCounters(db);
        const explainResult1 = coll
            .explain()
            .aggregate([{$match: {x: 1, z: 1}}, {$group: {_id: "$z"}}]);
        const explainResult2 = coll
            .explain()
            .aggregate([{$match: {x: 2, z: 2}}, {$group: {_id: "$z"}}]);
        const after = getCounters(db);

        // Check counters.
        assert.eq(after.leadingFilter - before.leadingFilter, 2);
        assert.eq(after.leadingFilterSimplified - before.leadingFilterSimplified, 2);

        // Both queries share the same plan shape with a FETCH residual filter over 'z'.
        for (const explainResult of [explainResult1, explainResult2]) {
            const fetchStage = getAggPlanStage(explainResult, "FETCH");
            assert(fetchStage && fetchStage.filter && fetchStage.filter.z, fetchStage);
        }
    });

    it("no increments for an empty $match + $group", function () {
        const before = getCounters(db);
        coll.explain().aggregate([{$match: {}}, {$group: {_id: "$z"}}]);
        const after = getCounters(db);

        // Check counters.
        assert.eq(after.leadingFilter - before.leadingFilter, 0);
        assert.eq(after.leadingFilterSimplified - before.leadingFilterSimplified, 0);
    });

    it("increments leadingFilterSimplified for a $match + $lookup on indexed non-array paths", function () {
        const before = getCounters(db);
        const explainResult = coll.explain().aggregate([
            {$match: {x: 1, z: 1}},
            {
                $lookup: {
                    from: other.getName(),
                    localField: "z",
                    foreignField: "z",
                    as: "joined",
                },
            },
        ]);
        const after = getCounters(db);

        // Check counters.
        assert.eq(after.leadingFilter - before.leadingFilter, 1);
        assert.eq(after.leadingFilterSimplified - before.leadingFilterSimplified, 1);

        // Check that the plan has a FETCH residual filter over 'z'.
        const fetchStage = getAggPlanStage(explainResult, "FETCH");
        assert(fetchStage && fetchStage.filter && fetchStage.filter.z, fetchStage);
    });

    it("increments leadingFilterSimplified for a $match + $lookup + $unwind on indexed non-array paths", function () {
        const before = getCounters(db);
        const explainResult = coll.explain().aggregate([
            {$match: {x: 1, z: 1}},
            {
                $lookup: {
                    from: other.getName(),
                    localField: "z",
                    foreignField: "z",
                    as: "joined",
                },
            },
            {$unwind: "$joined"},
        ]);
        const after = getCounters(db);

        // Check counters.
        assert.eq(after.leadingFilter - before.leadingFilter, 1);
        assert.eq(after.leadingFilterSimplified - before.leadingFilterSimplified, 1);

        // Check that the plan has a FETCH residual filter over 'z'.
        const fetchStage = getAggPlanStage(explainResult, "FETCH");
        assert(fetchStage && fetchStage.filter && fetchStage.filter.z, fetchStage);
    });

    it("increments leadingFilterSimplified for a $or with two branches", function () {
        // Even with a $or with two branches, we still increment the per-query leadingFilter / leadingFilterSimplified a single time.
        const before = getCounters(db);
        const explainResult = coll.explain().aggregate([
            {
                $match: {
                    $or: [
                        {x: 1, z: 1},
                        {x: 2, z: 2},
                    ],
                },
            },
            {$group: {_id: "$z"}},
        ]);
        const after = getCounters(db);

        // Check counters.
        assert.eq(after.leadingFilter - before.leadingFilter, 1);
        assert.eq(after.leadingFilterSimplified - before.leadingFilterSimplified, 1);

        // Each $or branch produces its own FETCH with a 'z' residual filter.
        const fetchStages = getAggPlanStages(explainResult, "FETCH");
        assert.gt(fetchStages.length, 0);
        for (const fetchStage of fetchStages) {
            assert(fetchStage.filter && fetchStage.filter.z, fetchStage);
        }
    });
});
