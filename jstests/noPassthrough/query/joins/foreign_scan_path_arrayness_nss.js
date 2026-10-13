/**
 * Regression test: with join optimization on, a $match on a foreign field after $lookup + $unwind
 * must consult the foreign collection's PathArrayness metadata, not the base collection's. A
 * non-multikey index on a same-named field in the base collection must not cause the foreign scan
 * to elide traverseF and drop foreign documents whose field is an array. Each case is also run
 * with join optimization off so the classic $lookup path stays correct if it ever consults
 * PathArrayness.
 * @tags: [
 *   requires_sbe
 * ]
 */
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {joinOptUsed, usedIndexedNestLoopJoinEmbedding} from "jstests/libs/query/join_utils.js";

describe("foreign scan path arrayness", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagPathArrayness: true,
                internalEnableJoinOptimization: true,
            },
        });
        const db = this.conn.getDB(jsTestName());
        this.base = db.base;
        this.foreign = db.foreign;

        assert.commandWorked(this.base.insertMany([{_id: 1, k: 1, x: 5}]));
        assert.commandWorked(
            this.foreign.insertMany([
                {_id: 10, k: 1, x: [1, 2]},
                {_id: 11, k: 1, x: 3},
            ]),
        );
        // Register the join key in PathArrayness so join optimization accepts the pipeline.
        assert.commandWorked(this.base.createIndex({k: 1}));
        assert.commandWorked(this.foreign.createIndex({k: 1}));

        this.pipeline = [
            {$lookup: {from: this.foreign.getName(), localField: "k", foreignField: "k", as: "f"}},
            {$unwind: "$f"},
            {$match: {"f.x": 2}},
        ];
        this.expected = [{_id: 1, k: 1, x: 5, f: {_id: 10, k: 1, x: [1, 2]}}];
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    afterEach(function () {
        this.base.dropIndex({x: 1});
    });

    it("returns array-typed foreign matches with a non-multikey base index on the same field", function () {
        // The base index on "x" marks "x" as non-array in the base collection's metadata.
        assert.commandWorked(this.base.createIndex({x: 1}));

        const explain = this.base.explain().aggregate(this.pipeline);
        assert(joinOptUsed(explain), "join optimization did not fire", {explain});
        // Before the fix this returned [] because traverseF was elided for the foreign "x".
        assert.sameMembers(this.base.aggregate(this.pipeline).toArray(), this.expected);
    });

    it("returns array-typed foreign matches without the base index", function () {
        const explain = this.base.explain().aggregate(this.pipeline);
        assert(joinOptUsed(explain), "join optimization did not fire", {explain});
        assert.sameMembers(this.base.aggregate(this.pipeline).toArray(), this.expected);
    });

    it("returns array-typed foreign matches under a forced INLJ fetch", function () {
        // The base index on "x" marks "x" as non-array in the base collection's metadata.
        assert.commandWorked(this.base.createIndex({x: 1}));
        // Force an indexed nested loop join so the foreign $match becomes a residual filter on
        // a FetchNode over the foreign IndexProbeNode.
        assert.commandWorked(this.conn.adminCommand({setParameter: 1, internalJoinMethod: "INLJ"}));
        try {
            const explain = this.base.explain().aggregate(this.pipeline);
            assert(joinOptUsed(explain), "join optimization did not fire", {explain});
            assert(usedIndexedNestLoopJoinEmbedding(explain), "expected an INLJ plan", {explain});
            // Before the fetch-side fix this returned [] because traverseF was elided for the
            // foreign "x" using the base collection's PathArrayness metadata.
            assert.sameMembers(this.base.aggregate(this.pipeline).toArray(), this.expected);
        } finally {
            assert.commandWorked(
                this.conn.adminCommand({setParameter: 1, internalJoinMethod: "any"}),
            );
        }
    });

    describe("with join optimization off", function () {
        before(function () {
            assert.commandWorked(
                this.conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}),
            );
        });

        after(function () {
            assert.commandWorked(
                this.conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}),
            );
        });

        it("returns array-typed foreign matches with a non-multikey base index on the same field", function () {
            // The base index on "x" marks "x" as non-array in the base collection's metadata.
            assert.commandWorked(this.base.createIndex({x: 1}));

            const explain = this.base.explain().aggregate(this.pipeline);
            assert(!joinOptUsed(explain), "expected join optimization to be off", {explain});
            assert.sameMembers(this.base.aggregate(this.pipeline).toArray(), this.expected);
        });

        it("returns array-typed foreign matches without the base index", function () {
            const explain = this.base.explain().aggregate(this.pipeline);
            assert(!joinOptUsed(explain), "expected join optimization to be off", {explain});
            assert.sameMembers(this.base.aggregate(this.pipeline).toArray(), this.expected);
        });
    });
});
