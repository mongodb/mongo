/**
 * Tests that a join query whose single-table access plan is EOF (i.e. a trivially false predicate)
 * runs correctly with the join plan cache enabled. Such a plan carries no 'SolutionCacheData' and
 * must not be written to the join plan cache. Regression test for SERVER-130315.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("join plan cache with an EOF access path", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                internalEnableJoinOptimization: true,
                internalEnableJoinPlanCache: true,
            },
        });

        const db = this.conn.getDB(jsTestName());
        this.baseColl = db[jsTestName()];
        this.foreignColl = db[jsTestName() + "_foreign"];

        assert.commandWorked(this.baseColl.insertMany([{joinKey: 1}, {joinKey: 2}]));
        assert.commandWorked(this.foreignColl.insertMany([{joinKey: 1}, {joinKey: 2}]));

        // Join optimization only engages when the join fields are indexed.
        assert.commandWorked(this.baseColl.createIndex({joinKey: 1}));
        assert.commandWorked(this.foreignColl.createIndex({joinKey: 1}));

        this.lookup = {
            $lookup: {
                from: this.foreignColl.getName(),
                localField: "joinKey",
                foreignField: "joinKey",
                as: "out",
            },
        };
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    // Each case is run twice: the first execution populates the cache (or declines to), the second
    // would consult it. Both must succeed and return no documents.
    function assertReturnsNothing(coll, pipeline) {
        for (let i = 0; i < 2; i++) {
            assert.eq(coll.aggregate(pipeline).toArray(), [], "run " + i, {pipeline});
        }
    }

    it("handles an EOF access path on the foreign side", function () {
        const lookup = Object.assign({}, this.lookup);
        lookup.$lookup = Object.assign({}, lookup.$lookup, {
            pipeline: [{$match: {$alwaysFalse: 1}}],
        });
        assertReturnsNothing(this.baseColl, [lookup, {$unwind: "$out"}]);
    });

    it("handles an EOF access path on the local side", function () {
        assertReturnsNothing(this.baseColl, [
            {$match: {$alwaysFalse: 1}},
            this.lookup,
            {$unwind: "$out"},
        ]);
    });

    it("handles an EOF access path on both sides", function () {
        const lookup = Object.assign({}, this.lookup);
        lookup.$lookup = Object.assign({}, lookup.$lookup, {
            pipeline: [{$match: {$alwaysFalse: 1}}],
        });
        assertReturnsNothing(this.baseColl, [
            {$match: {$alwaysFalse: 1}},
            lookup,
            {$unwind: "$out"},
        ]);
    });

    it("still caches plans for a join with no EOF access path", function () {
        assert.eq(this.baseColl.aggregate([this.lookup, {$unwind: "$out"}]).toArray().length, 2);
        assert.eq(this.baseColl.aggregate([this.lookup, {$unwind: "$out"}]).toArray().length, 2);
    });
});
