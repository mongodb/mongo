/**
 * Tests the scenario where querying a view whose pipeline contains another view applies the view
 * pipelines correctly.
 *
 * The non-$match assertions below use $skip and $count so they actually expose any
 * double-apply — $match stages alone are idempotent and would mask a regression.
 *
 * @tags: [assumes_no_implicit_collection_creation_after_drop]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

describe("view-of-view subpipeline resolution", function () {
    before(function () {
        this.testDB = db.getSiblingDB("view_of_view_subpipeline_db");
        this.testDB.dropDatabase();

        const testDB = this.testDB;

        assert.commandWorked(
            testDB.coll.insert([
                {_id: 1, a: true, b: false},
                {_id: 2, a: false, b: true},
                {_id: 3, a: true, b: true},
                {_id: 4, a: false, b: false},
            ]),
        );

        // viewB: documents where b == true => {_id:2}, {_id:3}.
        assert.commandWorked(testDB.createView("viewB", "coll", [{$match: {b: true}}]));

        // viewA: a == true unioned with viewB => [{_id:1}, {_id:3}, {_id:2}, {_id:3}] (no dedup).
        assert.commandWorked(testDB.createView("viewA", "coll", [{$match: {a: true}}, {$unionWith: "viewB"}]));

        // viewC: three-level chain. b == false unioned with viewA => 6 docs (no dedup).
        assert.commandWorked(testDB.createView("viewC", "coll", [{$match: {b: false}}, {$unionWith: "viewA"}]));

        // viewSkip: drops the first doc => 3 of 4.
        assert.commandWorked(testDB.createView("viewSkip", "coll", [{$skip: 1}]));

        // viewCount: returns {n: 2}.
        assert.commandWorked(testDB.createView("viewCount", "coll", [{$match: {b: true}}, {$count: "n"}]));

        // viewSkipUnionOuter: coll unioned with viewSkip via a user pipeline. The $set tags the
        // subpipe-side docs so we can isolate them from the outer side (which runs over coll
        // itself and also contains a:true docs).
        assert.commandWorked(
            testDB.createView("viewSkipUnionOuter", "coll", [
                {
                    $unionWith: {
                        coll: "viewSkip",
                        pipeline: [{$match: {a: true}}, {$set: {tag: "sub"}}],
                    },
                },
            ]),
        );

        // viewCountUnionOuter: coll unioned with viewCount, with a user pipeline that consumes
        // the count.
        assert.commandWorked(
            testDB.createView("viewCountUnionOuter", "coll", [
                {$unionWith: {coll: "viewCount", pipeline: [{$set: {y: "$n"}}]}},
            ]),
        );

        // viewSkipLookupOuter: $lookup against viewSkip with a user pipeline that filters and
        // tags. Single-apply on the subpipe is [$skip 1, $match a:true, $set tag:"sub"] over
        // coll.
        assert.commandWorked(
            testDB.createView("viewSkipLookupOuter", "coll", [
                {
                    $lookup: {
                        from: "viewSkip",
                        as: "joined",
                        pipeline: [{$match: {a: true}}, {$set: {tag: "sub"}}],
                    },
                },
            ]),
        );

        // viewCountLookupOuter: $lookup against viewCount with a user pipeline consuming the
        // count. Single-apply: each input gets joined: [{n:2, y:2}].
        assert.commandWorked(
            testDB.createView("viewCountLookupOuter", "coll", [
                {$lookup: {from: "viewCount", as: "joined", pipeline: [{$set: {y: "$n"}}]}},
            ]),
        );

        // hierColl: a small adjacency graph for $graphLookup.
        assert.commandWorked(
            testDB.hierColl.insert([
                {_id: "root", parent: null, active: true},
                {_id: "child1", parent: "root", active: true},
                {_id: "child2", parent: "root", active: false},
                {_id: "grandchild1", parent: "child1", active: true},
                {_id: "grandchild2", parent: "child1", active: false},
            ]),
        );

        // viewActive: hierColl filtered by active==true. Three docs.
        assert.commandWorked(testDB.createView("viewActive", "hierColl", [{$match: {active: true}}]));

        // viewGraphLookupOuter: a view that $graphLookup-s against viewActive. The $match
        // active:true is applied at every recursion step, so only active ancestors appear.
        assert.commandWorked(
            testDB.createView("viewGraphLookupOuter", "hierColl", [
                {
                    $graphLookup: {
                        from: "viewActive",
                        startWith: "$parent",
                        connectFromField: "parent",
                        connectToField: "_id",
                        as: "ancestors",
                    },
                },
            ]),
        );
    });

    after(function () {
        this.testDB.dropDatabase();
    });

    describe("$unionWith", function () {
        it("queries a view whose pipeline contains $unionWith on another view", function () {
            const result = this.testDB.viewA.aggregate([]).toArray();
            assertArrayEq({
                actual: result.map((d) => d._id),
                expected: [1, 3, 2, 3],
                extraErrorMsg: "viewA did not return expected union of a==true and b==true docs",
            });
        });

        it("queries a non-view base unioning a view-of-view", function () {
            const result = this.testDB.coll.aggregate([{$unionWith: "viewA"}]).toArray();
            assert.eq(
                result.length,
                8,
                "coll $unionWith viewA should produce 8 docs but got: " + tojson(result.map((d) => d._id)),
            );
        });

        it("handles a three-level view chain", function () {
            const result = this.testDB.viewC.aggregate([]).toArray();
            assertArrayEq({
                actual: result.map((d) => d._id),
                expected: [1, 4, 1, 3, 2, 3],
                extraErrorMsg: "viewC did not return expected three-level union result",
            });
        });

        it("does not double-apply $skip on a view-targeted subpipeline", function () {
            // Single-apply: [$skip 1, $match a:true, $set tag] => 1 doc.
            // Double-apply: [$skip 1, $skip 1, $match a:true, $set tag] => 0 docs.
            const expected = this.testDB.coll.aggregate([{$skip: 1}, {$match: {a: true}}]).toArray().length;
            const result = this.testDB.viewSkipUnionOuter.aggregate([]).toArray();
            const subpipeSide = result.filter((d) => d.tag === "sub").map((d) => d._id);
            assert.eq(
                subpipeSide.length,
                expected,
                "viewSkipUnionOuter: expected " +
                    expected +
                    " docs from the subpipe side, got " +
                    subpipeSide.length +
                    " — a smaller count indicates viewSkip's [$skip 1] was double-applied. " +
                    "Result: " +
                    tojson(result.map((d) => d._id)),
            );
        });

        it("does not double-apply $count on a view-targeted subpipeline", function () {
            // Single-apply: [$match b, $count n, $set y=$n] => {n:2, y:2}.
            // Double-apply: [$match b, $count n, $match b, $count n, $set y=$n] => {n:0, y:0}.
            const result = this.testDB.viewCountUnionOuter.aggregate([]).toArray();
            const fromCountSide = result.filter((d) => d.n !== undefined);
            assert.eq(
                fromCountSide.length,
                1,
                "Expected exactly one doc from viewCount side, got: " + tojson(fromCountSide),
            );
            assert.eq(
                fromCountSide[0].n,
                2,
                "viewCount should report n: 2 (single-apply); got n: " + fromCountSide[0].n,
            );
            assert.eq(
                fromCountSide[0].y,
                2,
                "user-pipeline $set y=$n should produce y: 2; got y: " + fromCountSide[0].y,
            );
        });
    });

    describe("$lookup", function () {
        it("does not double-apply $count on a view-targeted lookup subpipeline", function () {
            // Single-apply: each input gets joined: [{n:2, y:2}].
            // Double-apply: joined: [{n:0, y:0}].
            const result = this.testDB.viewCountLookupOuter.aggregate([]).toArray();
            assert.eq(result.length, 4, "viewCountLookupOuter should return 4 docs (one per coll doc)");
            for (const doc of result) {
                assert.eq(doc.joined.length, 1, "Each doc should have exactly one joined entry; got: " + tojson(doc));
                assert.eq(
                    doc.joined[0].n,
                    2,
                    "viewCount lookup should report n: 2 (single-apply); got n: " +
                        doc.joined[0].n +
                        " in: " +
                        tojson(doc),
                );
                assert.eq(
                    doc.joined[0].y,
                    2,
                    "user-pipeline $set y=$n in lookup should produce y: 2; got y: " +
                        doc.joined[0].y +
                        " in: " +
                        tojson(doc),
                );
            }
        });

        it("applies a view-targeted lookup subpipeline once on $skip", function () {
            // Verify each input doc's `joined` matches the manually-equivalent pipeline.
            const expected = this.testDB.coll
                .aggregate([{$skip: 1}, {$match: {a: true}}])
                .toArray()
                .map((d) => d._id)
                .sort();
            const result = this.testDB.viewSkipLookupOuter.aggregate([]).toArray();
            for (const doc of result) {
                const joinedIds = doc.joined.map((d) => d._id).sort();
                assert.eq(
                    joinedIds,
                    expected,
                    "viewSkipLookupOuter: each doc's joined ids should equal " +
                        tojson(expected) +
                        ", got: " +
                        tojson(joinedIds) +
                        " for doc " +
                        tojson(doc),
                );
                for (const j of doc.joined) {
                    assert.eq(j.tag, "sub", "joined doc should carry tag from user pipeline: " + tojson(j));
                }
            }
        });
    });

    describe("$graphLookup", function () {
        it("applies the target view's filter at every recursion step", function () {
            // For grandchild1 (active:true, parent:child1), $graphLookup against viewActive
            // should walk through 'child1' and 'root'. Inactive docs in the underlying
            // hierColl must not appear.
            const result = this.testDB.viewGraphLookupOuter.aggregate([{$match: {_id: "grandchild1"}}]).toArray();
            assert.eq(result.length, 1, "should match exactly one doc for _id 'grandchild1'");
            const ancestorIds = result[0].ancestors.map((d) => d._id).sort();
            assert.eq(
                ancestorIds,
                ["child1", "root"],
                "grandchild1 should walk through 'child1' and 'root'; got: " + tojson(ancestorIds),
            );
            for (const a of result[0].ancestors) {
                assert.eq(a.active, true, "graphLookup against viewActive returned inactive doc: " + tojson(a));
            }
        });
    });
});
