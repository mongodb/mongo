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
