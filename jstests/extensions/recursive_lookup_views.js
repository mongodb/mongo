/**
 * Tests recursive/nested join operators where views and extension stages appear at multiple
 * levels: a $lookup inside a view definition, a $lookup whose subpipeline contains another
 * $lookup onto a view, and the $unionWith and $graphLookup equivalents.
 *
 * The risk is a view or a desugared expansion being applied zero times or twice when a subpipeline
 * is rebuilt at execution time. Counts below make both under- and over-application visible.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionStubParsers,
 *   featureFlagExtensionsInsideHybridSearch,
 *   requires_fcv_90,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {afterEach, before, describe, it} from "jstests/libs/mochalite.js";

const localCollName = jsTestName() + "_local";
const midCollName = jsTestName() + "_mid";
const leafCollName = jsTestName() + "_leaf";

describe("recursive and nested joins with views and extensions", function () {
    let localColl, midColl, leafColl;
    let createdViews;

    before(function () {
        localColl = db[localCollName];
        midColl = db[midCollName];
        leafColl = db[leafCollName];
        localColl.drop();
        midColl.drop();
        leafColl.drop();
        assert.commandWorked(localColl.insertMany([{_id: 1, key: "K"}]));
        assert.commandWorked(
            midColl.insertMany([
                {_id: 10, key: "K", name: "m1", val: 10},
                {_id: 11, key: "K", name: "m2", val: 20},
            ]),
        );
        assert.commandWorked(
            leafColl.insertMany([
                {_id: 20, key: "K", name: "l1", val: 100},
                {_id: 21, key: "K", name: "l2", val: 200},
            ]),
        );
        createdViews = [];
    });

    afterEach(function () {
        for (const viewName of createdViews.reverse()) {
            assertDropCollection(db, viewName);
        }
        createdViews = [];
    });

    function makeView(name, source, pipeline) {
        assert.commandWorked(db.createView(name, source, pipeline));
        createdViews.push(name);
        return name;
    }

    it("$lookup onto a view whose definition contains a plain $lookup", function () {
        const viewName = makeView(jsTestName() + "_v_inner_lookup", midCollName, [
            {$lookup: {from: leafCollName, localField: "key", foreignField: "key", as: "leaves"}},
        ]);

        const results = localColl
            .aggregate([{$lookup: {from: viewName, pipeline: [], as: "joined"}}])
            .toArray();
        assert.eq(results.length, 1, {results});
        assert.eq(results[0].joined.length, 2, "both mid docs", {results});
        for (const doc of results[0].joined) {
            assert.eq(doc.leaves.length, 2, "view's inner $lookup joins both leaf docs", {doc});
        }
    });

    it("$lookup onto a view whose definition contains a $lookup with a desugaring extension", function () {
        // The desugarer sits in the inner $lookup's subpipeline, two levels down.
        const viewName = makeView(jsTestName() + "_v_inner_desugar", midCollName, [
            {
                $lookup: {
                    from: leafCollName,
                    pipeline: [{$matchTopN: {filter: {}, sort: {val: 1}, limit: 1}}],
                    as: "leaves",
                },
            },
        ]);

        const results = localColl
            .aggregate([{$lookup: {from: viewName, pipeline: [], as: "joined"}}])
            .toArray();
        assert.eq(results.length, 1, {results});
        assert.eq(results[0].joined.length, 2, {results});
        for (const doc of results[0].joined) {
            assert.eq(doc.leaves.length, 1, "inner desugarer limits to 1 leaf", {doc});
            assert.eq(doc.leaves[0].name, "l1", {doc});
        }
    });

    it("recursive $lookup: subpipeline contains a nested $lookup onto a view", function () {
        // The nested view must be resolved and applied exactly once even though the outer
        // subpipeline is rebuilt.
        const leafViewName = makeView(jsTestName() + "_v_leaf", leafCollName, [
            {$matchTopN: {filter: {}, sort: {val: 1}, limit: 1}},
        ]);

        const results = localColl
            .aggregate([
                {
                    $lookup: {
                        from: midCollName,
                        pipeline: [
                            {
                                $lookup: {
                                    from: leafViewName,
                                    localField: "key",
                                    foreignField: "key",
                                    as: "leaves",
                                },
                            },
                        ],
                        as: "joined",
                    },
                },
            ])
            .toArray();
        assert.eq(results.length, 1, {results});
        assert.eq(results[0].joined.length, 2, {results});
        for (const doc of results[0].joined) {
            assert.eq(
                doc.leaves.length,
                1,
                "nested $lookup onto a desugaring view must yield exactly 1 leaf",
                {doc},
            );
            assert.eq(doc.leaves[0].name, "l1", {doc});
        }
    });

    it("recursive $lookup three levels deep, with a view at each level", function () {
        const leafViewName = makeView(jsTestName() + "_v3_leaf", leafCollName, [
            {$testBar: {noop: true}},
        ]);
        const midViewName = makeView(jsTestName() + "_v3_mid", midCollName, [
            {$matchTopN: {filter: {}, sort: {val: 1}, limit: 2}},
        ]);

        const results = localColl
            .aggregate([
                {
                    $lookup: {
                        from: midViewName,
                        pipeline: [
                            {
                                $lookup: {
                                    from: leafViewName,
                                    pipeline: [{$testBar: {deep: true}}],
                                    as: "leaves",
                                },
                            },
                        ],
                        as: "joined",
                    },
                },
            ])
            .toArray();
        assert.eq(results.length, 1, {results});
        assert.eq(results[0].joined.length, 2, {results});
        for (const doc of results[0].joined) {
            assert.eq(doc.leaves.length, 2, "leaf view yields both docs", {doc});
        }
    });

    it("recursive $unionWith: subpipeline contains a nested $unionWith onto a desugaring view", function () {
        const leafViewName = makeView(jsTestName() + "_vu_leaf", leafCollName, [
            {$matchTopN: {filter: {}, sort: {val: 1}, limit: 1}},
        ]);

        const results = localColl
            .aggregate([
                {$match: {__never_matches__: 1}},
                {
                    $unionWith: {
                        coll: midCollName,
                        pipeline: [{$unionWith: {coll: leafViewName, pipeline: []}}],
                    },
                },
            ])
            .toArray();
        // 2 mid docs + 1 doc from the desugaring leaf view.
        assert.eq(results.length, 3, "2 mid + 1 leaf-view doc", {results});
        assert.eq(results.filter((d) => d.name === "l1").length, 1, {results});
    });

    it("$graphLookup nested inside a $lookup subpipeline, targeting a desugaring view", function () {
        const leafViewName = makeView(jsTestName() + "_vg_leaf", leafCollName, [
            {$matchTopN: {filter: {}, sort: {val: 1}, limit: 1}},
        ]);

        const results = localColl
            .aggregate([
                {
                    $lookup: {
                        from: midCollName,
                        pipeline: [
                            {
                                $graphLookup: {
                                    from: leafViewName,
                                    startWith: "$key",
                                    connectFromField: "__none__",
                                    connectToField: "key",
                                    as: "leaves",
                                },
                            },
                        ],
                        as: "joined",
                    },
                },
            ])
            .toArray();
        assert.eq(results.length, 1, {results});
        assert.eq(results[0].joined.length, 2, {results});
        for (const doc of results[0].joined) {
            assert.eq(doc.leaves.length, 1, "graphLookup onto desugaring view yields 1 doc", {doc});
            assert.eq(doc.leaves[0].name, "l1", {doc});
        }
    });

    it("$lookup onto a view whose definition contains a $graphLookup onto another view", function () {
        const leafViewName = makeView(jsTestName() + "_vgg_leaf", leafCollName, [
            {$testBar: {noop: true}},
        ]);
        const midViewName = makeView(jsTestName() + "_vgg_mid", midCollName, [
            {
                $graphLookup: {
                    from: leafViewName,
                    startWith: "$key",
                    connectFromField: "__none__",
                    connectToField: "key",
                    as: "leaves",
                },
            },
        ]);

        const results = localColl
            .aggregate([{$lookup: {from: midViewName, pipeline: [], as: "joined"}}])
            .toArray();
        assert.eq(results.length, 1, {results});
        assert.eq(results[0].joined.length, 2, {results});
        for (const doc of results[0].joined) {
            assert.eq(doc.leaves.length, 2, "inner $graphLookup reaches both leaf docs", {doc});
        }
    });

    it("nested view chain reached through a recursive $lookup applies each level exactly once", function () {
        // Both levels' $addFields are idempotent and the base's $matchTopN stays at 1 doc under
        // re-filtering, so assert on the `level` value, which reveals the order levels were
        // applied in.
        const baseViewName = makeView(jsTestName() + "_chain_base", leafCollName, [
            {$matchTopN: {filter: {}, sort: {val: 1}, limit: 1}},
            {$addFields: {level: "base"}},
        ]);
        const topViewName = makeView(jsTestName() + "_chain_top", baseViewName, [
            {$addFields: {level: "top"}},
        ]);

        const results = localColl
            .aggregate([
                {
                    $lookup: {
                        from: midCollName,
                        pipeline: [{$lookup: {from: topViewName, pipeline: [], as: "leaves"}}],
                        as: "joined",
                    },
                },
            ])
            .toArray();
        assert.eq(results.length, 1, {results});
        assert.eq(results[0].joined.length, 2, {results});
        for (const doc of results[0].joined) {
            assert.eq(doc.leaves.length, 1, "chain yields exactly 1 doc", {doc});
            assert.eq(doc.leaves[0].level, "top", "top view applied last, exactly once", {doc});
            assert.eq(doc.leaves[0].name, "l1", {doc});
        }
    });
});
