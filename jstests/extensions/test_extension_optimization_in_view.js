/**
 * Tests that extension rewrite-rule optimizations remain correct when an extension stage lives
 * inside (or is queried on top of) a view. The following extension stages and their applicable
 * optimization rules are exercised/tested:
 *   - $testVectorSearchOptimization (desugars to $testVectorSearch): REDUNDANT_SORT_REMOVAL,
 *     eraseStage, eraseExtensionLimit, applyPipelineBounds.
 *   - $readNDocuments (desugars to $produceIds): applyMatchPushdown, applyProjectPushdown,
 *     REDUNDANT_SORT_REMOVAL (when sortById establishes an {_id: 1} sort pattern).
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionsInsideHybridSearch,
 *   requires_fcv_82,
 * ]
 */

// Each rule is exercised across three view scenarios:
//   - "in view def": the extension and the stage it interacts with both live in the view pipeline.
//   - "on view"    : the extension lives in the view pipeline and the interacting stage is supplied
//                    by the user query on top of the view, so the rule must fire across the resolved
//                    view->user boundary.
//   - "nested view": the same as "on view", split across a base view and an outer view that references it.

import {before, afterEach, describe, it} from "jstests/libs/mochalite.js";
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getAggStagesAcrossSplitPipeline} from "jstests/libs/query/analyze_plan.js";

const buildOptStage = ({storedSource}) => {
    return {$testVectorSearchOptimization: {storedSource}};
};

const desugarFalseStage = {$testVectorSearchOptimization: {desugar: false}};

const sortStageVectorSearchScore = {$sort: {vectorSearchScore: {$meta: "vectorSearchScore"}}};

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb[jsTestName()];
const produceColl = testDb[jsTestName() + "_produce"];
let createdViews = [];
let viewCounter = 0;

// Maps each created view name to the flat pipeline its definition resolves to (for a nested view,
// the base view's resolved pipeline followed by the outer view's pipeline). assertViewMatchesResolved
// reads this to derive the equivalent non-view pipeline instead of having every caller restate it.
let viewResolvedPipeline = {};

function makeView(suffix, source, pipeline) {
    const name = jsTestName() + "_view_" + viewCounter++ + "_" + suffix;
    assert.commandWorked(testDb.createView(name, source, pipeline));
    createdViews.push(name);
    // If `source` is itself a view, prepend its resolved pipeline; a collection source has no entry
    // and contributes nothing.
    viewResolvedPipeline[name] = [...(viewResolvedPipeline[source] ?? []), ...pipeline];
    return name;
}

// Creates a base view (defined by `baseDef`) and an outer view (`outerDef`) that reads from it,
// returning the outer view's name.
function makeNestedView(suffix, source, baseDef, outerDef) {
    const baseViewName = makeView(suffix + "_base", source, baseDef);
    return makeView(suffix + "_outer", baseViewName, outerDef);
}

function dropCreatedViews() {
    // Drop in reverse insertion order so nested (outer) views are dropped before the base views
    // they reference.
    for (let i = createdViews.length - 1; i >= 0; i--) {
        assert(testDb[createdViews[i]].drop());
    }
    createdViews = [];
    viewResolvedPipeline = {};
}

// Runs queryPlanner explain for `userPipeline` against the view.
function explainView(viewName, userPipeline = []) {
    return testDb[viewName].explain("queryPlanner").aggregate(userPipeline);
}

// Returns the spec object of the first `stageName` occurrence in the explain, or undefined.
function getStageSpecFromExplain(explain, stageName) {
    const stages = getAggStagesAcrossSplitPipeline(explain, stageName);
    return stages.length > 0 ? stages[0][stageName] : undefined;
}

function getPipelineSuffixBoundsFromExplain(explain) {
    const {minBoundsType, maxBoundsType, extractedLimit} =
        getStageSpecFromExplain(explain, "$testVectorSearch")?.limit ?? {};
    return {minBoundsType, maxBoundsType, extractedLimit};
}

function isStagePresent(explain, stageName) {
    return getAggStagesAcrossSplitPipeline(explain, stageName).length > 0;
}

function assertStageAbsent(explain, stageName) {
    assert(!isStagePresent(explain, stageName), "expected stage to be erased", {
        stageName,
        explain,
    });
}

function assertStagePresent(explain, stageName) {
    assert(isStagePresent(explain, stageName), "expected stage to be present", {
        stageName,
        explain,
    });
}

// Executes `userPipeline` on the view and asserts it returns the same documents as the fully
// resolved pipeline (the view's resolved definition followed by the user pipeline) run directly on
// the base collection `baseColl`. This is the "final semantics match after view resolution" check: the
// optimization must not change which documents come back versus the equivalent non-view pipeline.
// Order-insensitive because several of these pipelines (e.g. sort-by-constant-score) have an
// arbitrary output order.
function assertViewMatchesResolved(viewName, userPipeline = [], baseColl = coll) {
    const viaView = testDb[viewName].aggregate(userPipeline).toArray();
    const resolved = baseColl
        .aggregate([...viewResolvedPipeline[viewName], ...userPipeline])
        .toArray();
    assertArrayEq({
        actual: viaView,
        expected: resolved,
        extraErrorMsg: `'${viewName}' via-view result differs from the resolved pipeline`,
    });
}

// Asserts the view (optionally with a user pipeline) returns exactly `expected`, order-insensitively.
function assertViewReturns(viewName, userPipeline, expected) {
    assertArrayEq({actual: testDb[viewName].aggregate(userPipeline).toArray(), expected});
}

describe("$testVectorSearchOptimization in views", function () {
    before(function () {
        coll.drop();
        assert.commandWorked(
            coll.insertMany([
                {_id: 1, x: 1},
                {_id: 2, x: 2},
                {_id: 3, x: 3},
            ]),
        );
    });

    afterEach(dropCreatedViews);

    describe("REDUNDANT_SORT_REMOVAL ($sort on vectorSearchScore) in a view", function () {
        it("erases sort when extension and sort are both in view definition", function () {
            // storedSource:false desugars to [$testVectorSearch, $_internalSearchIdLookup]; idLookup
            // preserves order and metadata, so REDUNDANT_SORT_REMOVAL walks past it to
            // $testVectorSearch's sort pattern and erases the $sort.
            const def = [buildOptStage({storedSource: false}), sortStageVectorSearchScore];
            const viewName = makeView("sort_in_def", coll.getName(), def);
            assertStageAbsent(explainView(viewName), "$sort");
            assertViewMatchesResolved(viewName);
        });

        it("erases sort across the view boundary", function () {
            // Use desugarFalseStage so the resolved suffix is [$testVectorSearch, $sort] with no
            // intervening desugared stage (an intervening stage would block erasure; see
            // test_vector_search_optimization.js and SERVER-127594).
            const viewName = makeView("sort_across_boundary", coll.getName(), [desugarFalseStage]);
            assertStageAbsent(explainView(viewName, [sortStageVectorSearchScore]), "$sort");
            assertViewMatchesResolved(viewName, [sortStageVectorSearchScore]);
        });

        it("erases sort in nested view", function () {
            // Extension lives in the base view; the $sort(vectorSearchScore) lives in the outer
            // view, so REDUNDANT_SORT_REMOVAL must fire across the resolved nested-view boundary.
            const viewName = makeNestedView(
                "sort_nested",
                coll.getName(),
                [buildOptStage({storedSource: false})],
                [sortStageVectorSearchScore],
            );
            assertStageAbsent(explainView(viewName), "$sort");
            assertViewMatchesResolved(viewName);
        });
    });

    describe("applyPipelineBounds limit extraction in a view", function () {
        function assertBounds(explain, expected) {
            const {minBoundsType, maxBoundsType, extractedLimit} =
                getPipelineSuffixBoundsFromExplain(explain);
            assert.eq(minBoundsType, expected.minBoundsType, "wrong minBoundsType", {explain});
            assert.eq(maxBoundsType, expected.maxBoundsType, "wrong maxBoundsType", {explain});
            assert.eq(extractedLimit, expected.extractedLimit, "wrong extractedLimit", {explain});
        }

        it("discrete bounds when $limit follows extension in view def", function () {
            const def = [desugarFalseStage, {$limit: 10}];
            const viewName = makeView("bounds_limit_in_def", coll.getName(), def);
            assertBounds(explainView(viewName), {
                minBoundsType: "discrete",
                maxBoundsType: "discrete",
                extractedLimit: 10,
            });
            assertViewMatchesResolved(viewName);
        });

        it("discrete bounds across the view boundary", function () {
            const viewName = makeView("bounds_limit_user", coll.getName(), [desugarFalseStage]);
            assertBounds(explainView(viewName, [{$limit: 7}]), {
                minBoundsType: "discrete",
                maxBoundsType: "discrete",
                extractedLimit: 7,
            });
            assertViewMatchesResolved(viewName, [{$limit: 7}]);
        });

        it("discrete bounds when $limit is in outer of nested view", function () {
            const viewName = makeNestedView(
                "bounds_nested",
                coll.getName(),
                [desugarFalseStage],
                [{$limit: 6}],
            );
            assertBounds(explainView(viewName), {
                minBoundsType: "discrete",
                maxBoundsType: "discrete",
                extractedLimit: 6,
            });
            assertViewMatchesResolved(viewName);
        });
    });

    describe("eraseStage rule in a view", function () {
        it("erases $project after extension within view def", function () {
            const def = [desugarFalseStage, {$project: {_id: 1}}];
            const viewName = makeView("erase_project_in_def", coll.getName(), def);
            assertStageAbsent(explainView(viewName), "$project");
            assertViewMatchesResolved(viewName);
        });

        it("erases $project across the view boundary", function () {
            const viewName = makeView("erase_project_user", coll.getName(), [desugarFalseStage]);
            assertStageAbsent(explainView(viewName, [{$project: {_id: 1}}]), "$project");
            assertViewMatchesResolved(viewName, [{$project: {_id: 1}}]);
        });

        it("erases $project after extension in nested view", function () {
            const viewName = makeNestedView(
                "erase_project_nested",
                coll.getName(),
                [desugarFalseStage],
                [{$project: {_id: 1}}],
            );
            assertStageAbsent(explainView(viewName), "$project");
            assertViewMatchesResolved(viewName);
        });
    });

    describe("eraseExtensionLimit rule in a view", function () {
        it("erases $extensionLimit at position 2 in view def", function () {
            const def = [desugarFalseStage, {$addFields: {a: 1}}, {$extensionLimit: 3}];
            const viewName = makeView("erase_extlimit_in_def", coll.getName(), def);
            const explain = explainView(viewName);
            assertStageAbsent(explain, "$extensionLimit");
            assertStagePresent(explain, "$addFields");
            assertViewMatchesResolved(viewName);
        });

        it("erases $extensionLimit when split across view boundary ($addFields in view, $extensionLimit from user)", function () {
            const viewName = makeView("erase_extlimit_split", coll.getName(), [
                desugarFalseStage,
                {$addFields: {a: 1}},
            ]);
            const explain = explainView(viewName, [{$extensionLimit: 3}]);
            assertStageAbsent(explain, "$extensionLimit");
            assertStagePresent(explain, "$addFields");
            assertViewMatchesResolved(viewName, [{$extensionLimit: 3}]);
        });

        it("erases $extensionLimit when split across nested view boundary", function () {
            const viewName = makeNestedView(
                "erase_extlimit_nested",
                coll.getName(),
                [desugarFalseStage, {$addFields: {a: 1}}],
                [{$extensionLimit: 3}],
            );
            const explain = explainView(viewName);
            assertStageAbsent(explain, "$extensionLimit");
            assertStagePresent(explain, "$addFields");
            assertViewMatchesResolved(viewName);
        });
    });

    describe("optimization fires when extension + interacting stage are both in the user pipeline, run on an unrelated view", function () {
        const viewPrefixes = [
            {suffix: "match_prefix", pipeline: [{$match: {x: {$gte: 1}}}]},
            {
                suffix: "addfields_match_prefix",
                pipeline: [{$addFields: {y: 1}}, {$match: {x: {$lte: 3}}}],
            },
        ];

        for (const {suffix, pipeline} of viewPrefixes) {
            it(`REDUNDANT_SORT_REMOVAL fires over view prefix [${suffix}]`, function () {
                const viewName = makeView("user_sort_over_" + suffix, coll.getName(), pipeline);
                const user = [desugarFalseStage, sortStageVectorSearchScore];
                assertStageAbsent(explainView(viewName, user), "$sort");
                assertViewMatchesResolved(viewName, user);
            });

            it(`eraseStage fires over view prefix [${suffix}]`, function () {
                const viewName = makeView("user_erase_over_" + suffix, coll.getName(), pipeline);
                const user = [desugarFalseStage, {$project: {_id: 1}}];
                assertStageAbsent(explainView(viewName, user), "$project");
                assertViewMatchesResolved(viewName, user);
            });

            it(`applyPipelineBounds extracts the limit over view prefix [${suffix}]`, function () {
                const viewName = makeView("user_bounds_over_" + suffix, coll.getName(), pipeline);
                const user = [desugarFalseStage, {$limit: 5}];
                const {minBoundsType, maxBoundsType, extractedLimit} =
                    getPipelineSuffixBoundsFromExplain(explainView(viewName, user));
                assert.eq(minBoundsType, "discrete", "wrong minBoundsType");
                assert.eq(maxBoundsType, "discrete", "wrong maxBoundsType");
                assert.eq(extractedLimit, 5, "wrong extractedLimit");
                assertViewMatchesResolved(viewName, user);
            });

            it(`eraseExtensionLimit fires over view prefix [${suffix}]`, function () {
                const viewName = makeView("user_extlimit_over_" + suffix, coll.getName(), pipeline);
                const user = [desugarFalseStage, {$addFields: {a: 1}}, {$extensionLimit: 3}];
                const explain = explainView(viewName, user);
                assertStageAbsent(explain, "$extensionLimit");
                assertStagePresent(explain, "$addFields");
                assertViewMatchesResolved(viewName, user);
            });
        }
    });
});

describe("$readNDocuments / $produceIds in views", function () {
    // Documents looked up by $_internalSearchIdLookup for the ids $produceIds generates. Seeded so
    // that value === _id * 2 and label === "doc_<_id>", matching readNDocuments_pushdown.js.
    const numDocs = 5;
    const expectedDocs = Array.from({length: numDocs}, (_, i) => ({
        _id: i,
        value: i * 2,
        label: `doc_${i}`,
    }));

    const readN = (opts) => ({$readNDocuments: opts});

    before(function () {
        produceColl.drop();
        assert.commandWorked(produceColl.insertMany(expectedDocs));
    });

    afterEach(dropCreatedViews);

    function assertMatchPushedDown(explain, expectedStartId) {
        const spec = getStageSpecFromExplain(explain, "$produceIds");
        assert(spec !== undefined, "expected $produceIds spec in explain", {explain});
        // startId defaults to 0 and is only serialized when non-zero, so treat an absent startId as
        // 0 - otherwise asserting an expected fold of `_id >= 0` (startId 0) would spuriously fail.
        assert.eq(
            spec.startId ?? 0,
            expectedStartId,
            "expected applyMatchPushdown to fold the $match into startId",
            {
                spec,
            },
        );
    }

    function assertMatchNotPushedDown(explain) {
        const spec = getStageSpecFromExplain(explain, "$produceIds");
        assert(spec !== undefined, "expected $produceIds spec in explain", {explain});
        // startId defaults to 0 and is only serialized when non-zero, so an unset startId means the
        // rule did not fire.
        assert(!spec.startId, "expected applyMatchPushdown NOT to fire", {spec});
    }

    function assertProjectPushedDown(explain, {skipValue, skipLabel}) {
        const spec = getStageSpecFromExplain(explain, "$produceIds");
        assert(spec !== undefined, "expected $produceIds spec in explain", {explain});
        assert.eq(!!spec.skipValue, skipValue, "wrong skipValue from applyProjectPushdown", {spec});
        assert.eq(!!spec.skipLabel, skipLabel, "wrong skipLabel from applyProjectPushdown", {spec});
    }

    describe("applyMatchPushdown rule in a view", function () {
        const matchGte2 = {$match: {_id: {$gte: 2}}};
        const expectedGte2 = expectedDocs.filter((d) => d._id >= 2);

        it("folds $match into startId when both are in the view definition", function () {
            const viewName = makeView("match_in_def", produceColl.getName(), [
                readN({numDocs}),
                matchGte2,
            ]);
            assertMatchPushedDown(explainView(viewName), 2);
            assertViewReturns(viewName, [], expectedGte2);
        });

        it("folds $match into startId across the view boundary", function () {
            const viewName = makeView("match_on_view", produceColl.getName(), [readN({numDocs})]);
            assertMatchPushedDown(explainView(viewName, [matchGte2]), 2);
            assertViewReturns(viewName, [matchGte2], expectedGte2);
        });

        it("folds $match into startId in a nested view", function () {
            const viewName = makeNestedView(
                "match_nested",
                produceColl.getName(),
                [readN({numDocs})],
                [matchGte2],
            );
            assertMatchPushedDown(explainView(viewName), 2);
            assertViewReturns(viewName, [], expectedGte2);
        });

        it("does NOT fold a non-_id $match (filter on value)", function () {
            const matchValue = {$match: {value: {$gte: 4}}};
            const viewName = makeView("match_non_id", produceColl.getName(), [readN({numDocs})]);
            const explain = explainView(viewName, [matchValue]);
            assertMatchNotPushedDown(explain);
            assertStagePresent(explain, "$match");
            assertViewReturns(
                viewName,
                [matchValue],
                expectedDocs.filter((d) => d.value >= 4),
            );
        });
    });

    describe("applyProjectPushdown rule in a view", function () {
        const projectIdOnly = {$project: {_id: 1}};
        const expectedIdOnly = expectedDocs.map((d) => ({_id: d._id}));

        it("suppresses value and label when {_id:1} project is in the view definition", function () {
            const viewName = makeView("project_in_def", produceColl.getName(), [
                readN({numDocs}),
                projectIdOnly,
            ]);
            assertProjectPushedDown(explainView(viewName), {skipValue: true, skipLabel: true});
            assertViewReturns(viewName, [], expectedIdOnly);
        });

        it("suppresses value and label across the view boundary", function () {
            const viewName = makeView("project_on_view", produceColl.getName(), [readN({numDocs})]);
            assertProjectPushedDown(explainView(viewName, [projectIdOnly]), {
                skipValue: true,
                skipLabel: true,
            });
            assertViewReturns(viewName, [projectIdOnly], expectedIdOnly);
        });

        it("suppresses value and label in a nested view", function () {
            const viewName = makeNestedView(
                "project_nested",
                produceColl.getName(),
                [readN({numDocs})],
                [projectIdOnly],
            );
            assertProjectPushedDown(explainView(viewName), {skipValue: true, skipLabel: true});
            assertViewReturns(viewName, [], expectedIdOnly);
        });

        it("suppresses only label when {_id:1, value:1} project crosses the boundary", function () {
            const viewName = makeView("project_selective", produceColl.getName(), [
                readN({numDocs}),
            ]);
            const user = [{$project: {_id: 1, value: 1}}];
            assertProjectPushedDown(explainView(viewName, user), {
                skipValue: false,
                skipLabel: true,
            });
            assertViewReturns(
                viewName,
                user,
                expectedDocs.map((d) => ({_id: d._id, value: d.value})),
            );
        });
    });

    describe("REDUNDANT_SORT_REMOVAL ($sort on _id from sortById) in a view", function () {
        const sortById = {$sort: {_id: 1}};
        const expectedIdOrder = expectedDocs.map((d) => d._id);

        function assertSortRemovedAndOrdered(viewName, userPipeline) {
            assertStageAbsent(explainView(viewName, userPipeline), "$sort");
            const ids = testDb[viewName]
                .aggregate(userPipeline)
                .toArray()
                .map((d) => d._id);
            assert.eq(ids, expectedIdOrder, "expected ascending-by-_id order after sort removal", {
                ids,
            });
        }

        it("erases $sort when sortById extension and sort are both in view def", function () {
            const viewName = makeView("sortid_in_def", produceColl.getName(), [
                readN({numDocs, sortById: true}),
                sortById,
            ]);
            assertSortRemovedAndOrdered(viewName, []);
        });

        it("erases $sort across the view boundary", function () {
            const viewName = makeView("sortid_on_view", produceColl.getName(), [
                readN({numDocs, sortById: true}),
            ]);
            assertSortRemovedAndOrdered(viewName, [sortById]);
        });

        it("erases $sort in a nested view", function () {
            const viewName = makeNestedView(
                "sortid_nested",
                produceColl.getName(),
                [readN({numDocs, sortById: true})],
                [sortById],
            );
            assertSortRemovedAndOrdered(viewName, []);
        });

        it("does NOT erase $sort when sortById is not set (no sort pattern established)", function () {
            const viewName = makeView("sortid_no_pattern", produceColl.getName(), [
                readN({numDocs}),
            ]);
            assertStagePresent(explainView(viewName, [sortById]), "$sort");
            assert.eq(
                testDb[viewName]
                    .aggregate([sortById])
                    .toArray()
                    .map((d) => d._id),
                expectedIdOrder,
                "results must still be ordered by the retained $sort",
            );
        });
    });
});

// TODO SERVER-125839: Add coverage for optimization rules that inspect the stages preceding an
// extension stage (e.g. stages contributed by a view prefix) once extension stages gain the
// ability to look at earlier stages during optimization.
