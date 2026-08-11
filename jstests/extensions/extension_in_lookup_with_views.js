/**
 * End-to-end tests for extension stages in $lookup subpipelines, including when the
 * $lookup targets views containing extension stages.
 *
 * Covers: allowed transform/source/desugar extensions in $lookup; extension stages in view
 * definitions used by $lookup; multi-level view chains; and rejection of kNotAllowed extensions.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionStubParsers,
 *   featureFlagExtensionsInsideHybridSearch,
 *   requires_fcv_90,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

const localCollName = jsTestName() + "_local";
const foreignCollName = jsTestName() + "_foreign";

// TODO SERVER-128961 Most existing stages don't correctly implement AllowedInLookup stage
// constraints at LiteParsed time and so they hit the full DocumentSourceLookup validation
// code instead. When this is implemented, remove the 51047 code.
const kNotAllowedInLookupCode = [51047, 11725900];

describe("extension stages in $lookup with views", function () {
    let localColl, foreignColl;

    before(function () {
        localColl = db[localCollName];
        foreignColl = db[foreignCollName];
        localColl.drop();
        foreignColl.drop();
        assert.commandWorked(
            localColl.insertMany([
                {_id: 1, key: "A"},
                {_id: 2, key: "B"},
            ]),
        );
        assert.commandWorked(
            foreignColl.insertMany([
                {_id: 10, name: "x", val: 100},
                {_id: 11, name: "y", val: 200},
            ]),
        );
    });

    it("$loaf transform extension runs directly in $lookup subpipeline", function () {
        const results = localColl
            .aggregate([
                {
                    $lookup: {
                        from: foreignCollName,
                        pipeline: [{$loaf: {numSlices: 2}}],
                        as: "loafed",
                    },
                },
            ])
            .toArray();
        assert.eq(results.length, 2, "all local docs returned", {results});
        assert.gt(results[0].loafed.length, 0, "$loaf should produce output in subpipeline", {
            results,
        });
    });

    it("$toast source extension runs directly in $lookup subpipeline", function () {
        const results = localColl
            .aggregate([
                {
                    $lookup: {
                        from: foreignCollName,
                        pipeline: [{$toast: {temp: 350, numSlices: 1}}],
                        as: "toasted",
                    },
                },
            ])
            .toArray();
        assert.eq(results.length, 2, {results});
        assert.eq(
            results[0].toasted,
            [{slice: 0, isBurnt: false}],
            "$toast should produce 1 non-burnt slice",
            {
                results,
            },
        );
    });

    it("extension stage in view definition is allowed when $lookup targets that view", function () {
        const viewName = jsTestName() + "_ext_view";
        assert.commandWorked(db.createView(viewName, foreignCollName, [{$testBar: {tag: 1}}]));
        try {
            const results = localColl
                .aggregate([{$lookup: {from: viewName, pipeline: [], as: "joined"}}])
                .toArray();
            assert.eq(results.length, 2, {results});
            // $testBar is a no-op, so all 2 foreign docs should be joined.
            assert.eq(results[0].joined.length, 2, "should join all foreign docs", {results});
        } finally {
            assertDropCollection(db, viewName);
        }
    });

    it("$lookup from multi-level view chain resolves extension in base view correctly", function () {
        const baseViewName = jsTestName() + "_base_view";
        const topViewName = jsTestName() + "_top_view";
        assert.commandWorked(db.createView(baseViewName, foreignCollName, [{$testBar: {tag: 1}}]));
        assert.commandWorked(
            db.createView(topViewName, baseViewName, [{$addFields: {fromTop: true}}]),
        );
        try {
            const results = localColl
                .aggregate([{$lookup: {from: topViewName, pipeline: [], as: "chained"}}])
                .toArray();
            assert.eq(results.length, 2, {results});
            assert.eq(results[0].chained.length, 2, {results});
            assert(results[0].chained[0].fromTop === true, "view chain addFields should apply", {
                results,
            });
        } finally {
            assertDropCollection(db, topViewName);
            assertDropCollection(db, baseViewName);
        }
    });

    it("extension stage in outer pipeline and $lookup subpipeline operate independently", function () {
        const results = localColl
            .aggregate([
                {$testBar: {outer: 1}},
                {
                    $lookup: {
                        from: foreignCollName,
                        pipeline: [{$testBar: {inner: 1}}],
                        as: "joined",
                    },
                },
            ])
            .toArray();
        assert.eq(results.length, 2, {results});
        assert.eq(results[0].joined.length, 2, {results});
    });

    it("$addViewName in $lookup subpipeline receives view binding when from: is a view", function () {
        const viewName = jsTestName() + "_simple_view";
        assert.commandWorked(
            db.createView(viewName, foreignCollName, [{$addFields: {fromView: true}}]),
        );
        try {
            const results = localColl
                .aggregate([
                    {$lookup: {from: viewName, pipeline: [{$addViewName: {}}], as: "named"}},
                ])
                .toArray();
            assert.eq(results.length, 2, {results});
            assert.eq(results[0].named.length, 2, {results});
            // $addViewName binds to the view and stamps doc.viewName with the view's name.
            assert.eq(
                results[0].named[0].viewName,
                viewName,
                "view binding should stamp viewName",
                {
                    results,
                },
            );
        } finally {
            assertDropCollection(db, viewName);
        }
    });

    it("$matchTopN desugar extension runs directly in $lookup subpipeline", function () {
        const results = localColl
            .aggregate([
                {
                    $lookup: {
                        from: foreignCollName,
                        pipeline: [{$matchTopN: {filter: {name: "x"}, sort: {val: 1}, limit: 1}}],
                        as: "matched",
                    },
                },
            ])
            .toArray();
        assert.eq(results.length, 2, {results});
        assert.eq(results[0].matched.length, 1, "matchTopN limit=1 should return 1 doc", {results});
        assert.eq(results[0].matched[0].name, "x", {results});
    });

    it("$desugarAddViewName in $lookup subpipeline binds to the view after desugaring", function () {
        const viewName = jsTestName() + "_desugar_view";
        assert.commandWorked(
            db.createView(viewName, foreignCollName, [{$addFields: {fromView: true}}]),
        );
        try {
            // $desugarAddViewName expands into [$addViewName, $doNothingViewPolicy].
            // After desugaring, $addViewName receives view binding and stamps doc.viewName.
            const results = localColl
                .aggregate([
                    {
                        $lookup: {
                            from: viewName,
                            pipeline: [{$desugarAddViewName: {}}],
                            as: "desugared",
                        },
                    },
                ])
                .toArray();
            assert.eq(results.length, 2, {results});
            assert.eq(results[0].desugared.length, 2, {results});
            assert.eq(
                results[0].desugared[0].viewName,
                viewName,
                "desugared extension should bind to view and stamp viewName",
                {results},
            );
        } finally {
            assertDropCollection(db, viewName);
        }
    });

    it("$matchTopN desugar extension in view definition is allowed when $lookup targets the view", function () {
        const viewName = jsTestName() + "_matchTopN_view";
        assert.commandWorked(
            db.createView(viewName, foreignCollName, [
                {$matchTopN: {filter: {name: "x"}, sort: {val: 1}, limit: 1}},
            ]),
        );
        try {
            const results = localColl
                .aggregate([{$lookup: {from: viewName, pipeline: [], as: "matched"}}])
                .toArray();
            assert.eq(results.length, 2, {results});
            // View expands $matchTopN to [$match, $sort, $limit] — should return 1 filtered doc.
            assert.eq(results[0].matched.length, 1, "view should return 1 filtered doc", {results});
            assert.eq(results[0].matched[0].name, "x", {results});
        } finally {
            assertDropCollection(db, viewName);
        }
    });

    it("$desugarAddViewName in both view definition and $lookup subpipeline", function () {
        // The view definition's $desugarAddViewName is NOT view-bound (it is part of the view, so
        // there is no outer view to name) and must not stamp viewName. The subpipeline's copy IS
        // view-bound and must stamp the view's name. Both must desugar independently.
        const viewName = jsTestName() + "_both_desugar_view";
        assert.commandWorked(
            db.createView(viewName, foreignCollName, [
                {$desugarAddViewName: {}},
                {$addFields: {fromView: true}},
            ]),
        );
        try {
            const results = localColl
                .aggregate([
                    {
                        $lookup: {
                            from: viewName,
                            pipeline: [{$desugarAddViewName: {}}],
                            as: "joined",
                        },
                    },
                ])
                .toArray();
            assert.eq(results.length, 2, {results});
            assert.eq(results[0].joined.length, 2, "should join all foreign docs", {results});
            for (const doc of results[0].joined) {
                assert.eq(
                    doc.viewName,
                    viewName,
                    "subpipeline desugarer must stamp the resolved view name",
                    {doc},
                );
            }
        } finally {
            assertDropCollection(db, viewName);
        }
    });

    it("two different desugaring extensions, one in the view definition and one in the $lookup subpipeline", function () {
        // View: $matchTopN desugars to [$match, $sort, $limit] and yields exactly the 'x' doc.
        // Subpipeline: $desugarAddViewName desugars to [$addViewName, $doNothingViewPolicy].
        // Both expansions must survive the execution-time subpipeline rebuild.
        const viewName = jsTestName() + "_two_desugar_view";
        assert.commandWorked(
            db.createView(viewName, foreignCollName, [
                {$matchTopN: {filter: {name: "x"}, sort: {val: 1}, limit: 1}},
            ]),
        );
        try {
            const results = localColl
                .aggregate([
                    {
                        $lookup: {
                            from: viewName,
                            pipeline: [{$desugarAddViewName: {}}],
                            as: "joined",
                        },
                    },
                ])
                .toArray();
            assert.eq(results.length, 2, {results});
            // $desugarAddViewName is first and kDoNothing, so the view pipeline is not prepended:
            // the subpipeline sees the resolved view's own output. Assert on the view name stamp,
            // which is the behavior under test.
            assert.gt(results[0].joined.length, 0, "expected joined docs", {results});
            for (const doc of results[0].joined) {
                assert.eq(doc.viewName, viewName, "view name stamped by desugared stage", {doc});
            }
        } finally {
            assertDropCollection(db, viewName);
        }
    });

    it("$matchTopN in view definition combined with $matchTopN in $lookup subpipeline", function () {
        // Both levels use a kDefaultPrepend desugarer, so the view pipeline IS prepended and the
        // two expansions compose: view yields the 2 docs with val >= 100, subpipeline narrows to
        // the single 'x' doc.
        const viewName = jsTestName() + "_nested_matchTopN_view";
        assert.commandWorked(
            db.createView(viewName, foreignCollName, [
                {$matchTopN: {filter: {val: {$gte: 100}}, sort: {val: 1}, limit: 2}},
            ]),
        );
        try {
            const results = localColl
                .aggregate([
                    {
                        $lookup: {
                            from: viewName,
                            pipeline: [
                                {$matchTopN: {filter: {name: "x"}, sort: {val: 1}, limit: 1}},
                            ],
                            as: "joined",
                        },
                    },
                ])
                .toArray();
            assert.eq(results.length, 2, {results});
            assert.eq(
                results[0].joined.length,
                1,
                "view yields 2 docs; subpipeline narrows to the single 'x' doc",
                {results},
            );
            assert.eq(results[0].joined[0].name, "x", {results});
        } finally {
            assertDropCollection(db, viewName);
        }
    });

    it("$lookup with localField/foreignField against a view with a non-desugaring extension", function () {
        // localField/foreignField uses the equality-match rewrite rather than a user subpipeline,
        // so it resolves the view through a different path than the pipeline: syntax above.
        const viewName = jsTestName() + "_lff_noop_view";
        assert.commandWorked(db.createView(viewName, foreignCollName, [{$testBar: {tag: 1}}]));
        try {
            const results = localColl
                .aggregate([
                    {$lookup: {from: viewName, localField: "_id", foreignField: "_id", as: "j"}},
                    {$sort: {_id: 1}},
                ])
                .toArray();
            assert.eq(results.length, 2, {results});
            // localColl _ids are 1 and 2; foreign _ids are 10 and 11, so nothing matches.
            for (const doc of results) {
                assert.eq(doc.j.length, 0, "no _id overlap between local and foreign", {doc});
            }
        } finally {
            assertDropCollection(db, viewName);
        }
    });

    // TODO SERVER-133203: skipped to keep the waterfall green. $lookup against a view with a
    // desugaring stage returns incorrect results on any topology with a mongos (it passes on a
    // standalone mongod). Re-enable when that fix lands.
    it.skip("$lookup with localField/foreignField against a view with a desugaring extension", function () {
        // $matchTopN desugars inside the view; the join must see the post-desugar output.
        // Join on a field that actually overlaps so a wrong result is visible.
        const viewName = jsTestName() + "_lff_desugar_view";
        assert.commandWorked(
            db.createView(viewName, foreignCollName, [
                {$matchTopN: {filter: {name: "x"}, sort: {val: 1}, limit: 1}},
                {$addFields: {joinKey: "A"}},
            ]),
        );
        try {
            const results = localColl
                .aggregate([
                    {
                        $lookup: {
                            from: viewName,
                            localField: "key",
                            foreignField: "joinKey",
                            as: "j",
                        },
                    },
                    {$sort: {_id: 1}},
                ])
                .toArray();
            assert.eq(results.length, 2, {results});
            // Local doc _id 1 has key "A" and matches the single view doc; _id 2 has key "B".
            assert.eq(results[0].j.length, 1, "key 'A' should match the view's one doc", {results});
            assert.eq(results[0].j[0].name, "x", {results});
            assert.eq(results[1].j.length, 0, "key 'B' should match nothing", {results});
        } finally {
            assertDropCollection(db, viewName);
        }
    });

    it("$testFoo in view definition causes localField/foreignField $lookup on that view to be rejected", function () {
        // The allowedInLookup=false restriction must be enforced on the equality-match path too,
        // not only on the pipeline: path.
        const viewName = jsTestName() + "_lff_rejected_view";
        assert.commandWorked(db.createView(viewName, foreignCollName, [{$testFoo: {}}]));
        try {
            assert.commandFailedWithCode(
                db.runCommand({
                    aggregate: localCollName,
                    pipeline: [
                        {
                            $lookup: {
                                from: viewName,
                                localField: "_id",
                                foreignField: "_id",
                                as: "x",
                            },
                        },
                    ],
                    cursor: {},
                }),
                kNotAllowedInLookupCode,
            );
        } finally {
            assertDropCollection(db, viewName);
        }
    });

    it("$lookup with localField/foreignField and a desugaring extension in the user pipeline", function () {
        // Desugarer in the outer (user) pipeline rather than the view, with the equality-match
        // join path. Isolates the outer-pipeline dimension from the view dimension.
        const viewName = jsTestName() + "_lff_outer_desugar_view";
        assert.commandWorked(
            db.createView(viewName, foreignCollName, [{$addFields: {joinKey: "A"}}]),
        );
        try {
            const results = localColl
                .aggregate([
                    {$matchTopN: {filter: {}, sort: {_id: 1}, limit: 2}},
                    {
                        $lookup: {
                            from: viewName,
                            localField: "key",
                            foreignField: "joinKey",
                            as: "j",
                        },
                    },
                    {$sort: {_id: 1}},
                ])
                .toArray();
            assert.eq(results.length, 2, {results});
            assert.eq(results[0].j.length, 2, "key 'A' matches both view docs", {results});
            assert.eq(results[1].j.length, 0, "key 'B' matches nothing", {results});
        } finally {
            assertDropCollection(db, viewName);
        }
    });

    // TODO SERVER-133203: skipped to keep the waterfall green. Same bug as above, reproduced with a
    // server-side desugarer ($sortByCount) rather than an extension one. Re-enable when the fix
    // lands.
    it.skip("$lookup with localField/foreignField against a view with a $sortByCount desugaring stage", function () {
        // Mirrors SERVER-133203: the view yields only the top seller ("widget"), so a local doc
        // with product "gizmo" must not match anything.
        const salesCollName = jsTestName() + "_sales";
        const reportsCollName = jsTestName() + "_reports";
        const viewName = jsTestName() + "_top_seller_view";
        const salesColl = db[salesCollName];
        const reportsColl = db[reportsCollName];
        salesColl.drop();
        reportsColl.drop();
        assert.commandWorked(
            salesColl.insertMany([{product: "widget"}, {product: "widget"}, {product: "gizmo"}]),
        );
        assert.commandWorked(reportsColl.insert({product: "gizmo"}));
        assert.commandWorked(
            db.createView(viewName, salesCollName, [{$sortByCount: "$product"}, {$limit: 1}]),
        );
        try {
            // Sanity check: the view really does yield only "widget".
            const viewDocs = db[viewName].find().toArray();
            assert.eq(viewDocs.length, 1, "view should yield exactly the top seller", {viewDocs});
            assert.eq(viewDocs[0]._id, "widget", {viewDocs});

            const results = reportsColl
                .aggregate([
                    {
                        $lookup: {
                            from: viewName,
                            localField: "product",
                            foreignField: "_id",
                            as: "t",
                        },
                    },
                ])
                .toArray();
            assert.eq(results.length, 1, {results});
            assert.eq(
                results[0].t.length,
                0,
                "'gizmo' must not match: 'widget' is the only top seller",
                {results},
            );
        } finally {
            assertDropCollection(db, viewName);
            salesColl.drop();
            reportsColl.drop();
        }
    });

    it("$desugarFoo (desugar-only, expands into $testFoo with allowedInLookup=false) is rejected in $lookup subpipeline", function () {
        // $desugarFoo desugars into $testFoo, which has allowedInLookup=false. Verifies that
        // LiteParsedExpandable::isAllowedInLookupPipeline() propagates the restriction via
        // its all_of check on the expanded stages.
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: localCollName,
                pipeline: [
                    {$lookup: {from: foreignCollName, pipeline: [{$desugarFoo: {}}], as: "x"}},
                ],
                cursor: {},
            }),
            kNotAllowedInLookupCode,
        );
    });

    it("$testFoo (allowedInLookup=false) is rejected in $lookup subpipeline", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: localCollName,
                pipeline: [{$lookup: {from: foreignCollName, pipeline: [{$testFoo: {}}], as: "x"}}],
                cursor: {},
            }),
            kNotAllowedInLookupCode,
        );
    });

    it("$testFoo in view definition causes $lookup on that view to be rejected", function () {
        const viewName = jsTestName() + "_rejected_view";
        assert.commandWorked(db.createView(viewName, foreignCollName, [{$testFoo: {}}]));
        try {
            assert.commandFailedWithCode(
                db.runCommand({
                    aggregate: localCollName,
                    pipeline: [{$lookup: {from: viewName, pipeline: [], as: "x"}}],
                    cursor: {},
                }),
                kNotAllowedInLookupCode,
            );
        } finally {
            assertDropCollection(db, viewName);
        }
    });
});
