/**
 * This test verifies that the $addViewName extension stage correctly passes the view name
 * through the extension boundary (AstNode -> LogicalStage -> ExecAggStage) and adds
 * it as a field in output documents. It also verifies that the view pipeline is correctly appended when the first stage's ViewPolicy is kDefaultPrepend, and not prepended if it's kDoNothing.
 *
 * This test also verifies that the $disallowViews extension stage correctly uasserts when
 * used in a view context.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {before, describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();

function createView(viewName, viewPipeline) {
    assert.commandWorked(db.createView(viewName, collName, viewPipeline));
    return {
        view: db[viewName],
        viewName,
        viewNss: `${db.getName()}.${viewName}`,
    };
}

function dropView(viewName) {
    assert.commandWorked(db.runCommand({drop: viewName}));
}

function verifyViewName(results, expectedViewName) {
    results.forEach((doc, index) => {
        assert(doc.hasOwnProperty("viewName"), `Document ${index} should have viewName field: ${tojson(doc)}`);
        assert.eq(
            doc.viewName,
            expectedViewName,
            `Document ${index} should have viewName field equal to "${expectedViewName}": ${tojson(doc)}`,
        );
    });
}

function testViewNameOnView(view, pipeline, expectedViewName, viewPipeline, shouldPrepend) {
    const result = view.view.aggregate(pipeline).toArray();
    verifyViewName(result, expectedViewName);
    // Verify stages are in expected order
    assertViewHandledCorrectly(view, pipeline, viewPipeline, shouldPrepend);
}

function testViewNameWithTemporaryView(viewName, viewPipeline, pipeline, shouldPrepend) {
    const testView = createView(viewName, viewPipeline);
    testViewNameOnView(testView, pipeline, viewName, viewPipeline, shouldPrepend);
    dropView(testView.viewName);
}

function testDisallowViewsFails(viewName, pipeline) {
    assert.commandFailedWithCode(
        db.runCommand({
            aggregate: viewName,
            pipeline: pipeline,
            cursor: {},
        }),
        11507700,
    );
}

function verifyViewNameAbsent(results) {
    results.forEach((doc, index) => {
        assert(
            !doc.hasOwnProperty("viewName") || doc.viewName === "",
            `Document ${index} should not have viewName when $addViewName is in view def: ${tojson(doc)}`,
        );
    });
}

function testViewNameAbsentInView(viewName, viewPipeline, userPipeline, shouldPrepend) {
    const testView = createView(viewName, viewPipeline);
    const results = testView.view.aggregate(userPipeline).toArray();
    verifyViewNameAbsent(results);
    // Verify stages are in expected order
    assertViewHandledCorrectly(testView, userPipeline, viewPipeline, shouldPrepend);
    dropView(testView.viewName);
}

function testNestedViewsWithViewNameStage(suffix, userPipeStage) {
    const baseCollName = jsTestName() + suffix;
    const baseColl = db[baseCollName];
    baseColl.drop();
    assert.commandWorked(baseColl.insertMany([{x: 1}, {x: 2}, {x: 3}]));

    const innerViewPipe = [{$sort: {_id: 1}}];
    const innerViewName = `inner_nested_view${suffix}`;
    assert.commandWorked(db.createView(innerViewName, baseCollName, innerViewPipe));
    const innerViewNss = `${db.getName()}.${innerViewName}`;

    const outerViewPipe = [{$match: {_id: {$gte: 2}}}];
    const outerViewName = `outer_nested_view${suffix}`;
    assert.commandWorked(db.createView(outerViewName, innerViewName, outerViewPipe));
    const outerViewNss = `${db.getName()}.${outerViewName}`;

    const userPipe = [userPipeStage];
    const shouldPrepend = false; // ViewPolicy is kDoNothing

    const outerView = {view: db[outerViewName], viewName: outerViewName, viewNss: outerViewNss};
    // Verify outer view name is present when running on outerView
    testViewNameOnView(outerView, userPipe, outerViewName, innerViewPipe.concat(outerViewPipe), shouldPrepend);

    const innerView = {view: db[innerViewName], viewName: innerViewName, viewNss: innerViewNss};
    // Verify inner view name is present when running on innerView
    testViewNameOnView(innerView, userPipe, innerViewName, innerViewPipe, shouldPrepend);
    dropView(outerViewName);
    dropView(innerViewName);
}

/**
 * Asserts that the actual pipeline stages contain the expected stages in the correct order.
 * This checks that each stage at position i has the same stage name/type as the expected stage
 * at position i, without requiring exact equality of the entire stage objects.
 *
 * @param {Array} actualStages The actual pipeline stages to check.
 * @param {Array} expectedStages The expected pipeline stages.
 * @param {string} contextMessage Optional context message to include in error messages.
 */
function assertStagesInExpectedOrder(actualStages, expectedStages, contextMessage = "") {
    assert.eq(actualStages.length, expectedStages.length);
    for (let i = 0; i < expectedStages.length; i++) {
        const stageName = Object.keys(expectedStages[i])[0];
        const errorMsg = contextMessage
            ? `${contextMessage} Expected stage ${stageName} at position ${i}, but found: ${tojson(actualStages[i])}`
            : `Expected stage ${stageName} at position ${i}, but found: ${tojson(actualStages[i])}`;
        assert(actualStages[i].hasOwnProperty(stageName), errorMsg);
    }
}

/**
 * Expands desugar stages in a pipeline to their desugared form.
 * Currently handles $desugarAddViewName -> [$addViewName, $doNothingViewPolicy]
 *
 * @param {Array} pipeline - The pipeline to expand
 * @returns {Array} The pipeline with desugar stages expanded
 */
function expandDesugarStages(pipeline) {
    const expanded = [];
    for (const stage of pipeline) {
        const stageName = Object.keys(stage)[0];
        if (stageName === "$desugarAddViewName") {
            // $desugarAddViewName desugars into $addViewName + $doNothingViewPolicy
            expanded.push({$addViewName: {}});
            expanded.push({$doNothingViewPolicy: stage[stageName]});
        } else {
            expanded.push(stage);
        }
    }
    return expanded;
}

/**
 * Asserts that running explain on the user pipeline against a view produces an explain output
 * that matches expectations based on whether the view pipeline should be prepended.
 *
 * @param {Object} view - The view object (from createView)
 * @param {Array} userPipeline - The user pipeline to run
 * @param {Array} viewPipeline - The expected view pipeline stages
 * @param {boolean} shouldPrepend - If true, view pipeline should be prepended; if false, only user pipeline should appear
 */
function assertViewHandledCorrectly(view, userPipeline, viewPipeline, shouldPrepend) {
    const explain = assert.commandWorked(view.view.explain().aggregate(userPipeline));

    // Get the pipeline stages from explain output
    let pipelineStages = [];
    if (explain.stages) {
        pipelineStages = explain.stages;
    } else {
        assert(explain.shards);
        const shardKeys = Object.keys(explain.shards);
        pipelineStages = explain.shards[shardKeys[0]].stages;
    }

    // Skip internal stages ($cursor)
    let startIdx = 0;
    if (pipelineStages.length > 0 && pipelineStages[0].hasOwnProperty("$cursor")) {
        startIdx = 1;
    }

    const actualStages = pipelineStages.slice(startIdx);

    // Expand desugar stages in expected pipelines (explain shows desugared stages)
    const expandedUserPipeline = expandDesugarStages(userPipeline);

    let expectedStages;
    if (shouldPrepend) {
        // View pipeline should be prepended before user pipeline
        const expandedViewPipeline = expandDesugarStages(viewPipeline);
        expectedStages = expandedViewPipeline.concat(expandedUserPipeline);
        assertStagesInExpectedOrder(actualStages, expectedStages, "View pipeline should be prepended: ");
    } else {
        // Only user pipeline should appear (view pipeline not prepended)
        expectedStages = expandedUserPipeline;
        assertStagesInExpectedOrder(actualStages, expectedStages, "Only user pipeline should appear: ");
    }
}

describe("View policy extension stages", function () {
    let coll;

    before(function () {
        coll = db[collName];
        coll.drop();
        assert.commandWorked(coll.insertMany([{x: 1}, {x: 2}, {x: 3}]));
    });

    describe("Extension stage on view", function () {
        it("should add view name when $addViewName is used on a regular view", function () {
            const viewPipe = [{$addFields: {a: 1}}];
            const userPipe = [{$addViewName: {}}];
            const viewName = "test_view";
            const shouldPrepend = false; // ViewPolicy is kDoNothing
            testViewNameWithTemporaryView(viewName, viewPipe, userPipe, shouldPrepend);
        });

        it("should add view name when $addViewName is used on an extension view", function () {
            const viewPipe = [{$testFoo: {}}];
            const userPipe = [{$addViewName: {}}];
            const viewName = "foo_view";
            const shouldPrepend = false; // ViewPolicy is kDoNothing
            testViewNameWithTemporaryView(viewName, viewPipe, userPipe, shouldPrepend);
        });

        it("should add view name when $addViewName is used later in the pipeline", function () {
            const viewPipe = [{$addFields: {a: 1}}];
            const userPipe = [{$project: {_id: 1}}, {$addViewName: {}}];
            const viewName = "test_view_later";
            const shouldPrepend = true; // ViewPolicy is kDefaultPrepend
            testViewNameWithTemporaryView(viewName, viewPipe, userPipe, shouldPrepend);
        });

        it("should add view name when $desugarAddViewName is used", function () {
            const viewPipe = [{$addFields: {a: 1}}];
            const userPipe = [{$project: {_id: 1}}, {$desugarAddViewName: {}}];
            const viewName = "test_view_desugar";
            const shouldPrepend = true; // ViewPolicy is kDefaultPrepend
            testViewNameWithTemporaryView(viewName, viewPipe, userPipe, shouldPrepend);
        });

        it("should assert when $disallowViews is used", function () {
            const viewPipe = [{$addFields: {a: 1}}];
            const userPipe = [{$disallowViews: {}}];
            const view = createView("test_view_disallow", viewPipe);
            testDisallowViewsFails(view.viewName, userPipe);
            dropView(view.viewName);
        });

        it("should assert when $disallowViews is used later in pipeline", function () {
            const viewPipe = [{$addFields: {a: 1}}];
            const userPipe = [{$project: {_id: 0}}, {$disallowViews: {}}];
            const view = createView("test_view_disallow_later", viewPipe);
            testDisallowViewsFails(view.viewName, userPipe);
            dropView(view.viewName);
        });

        it("should assert when multiple extension stages with different policies are used - first allows, second allows and applies view policy, third disallows", function () {
            const viewPipe = [{$addFields: {a: 1}}];
            const userPipe = [{$testFoo: {}}, {$addViewName: {}}, {$disallowViews: {}}];
            const view = createView("multi_stage_view", viewPipe);
            testDisallowViewsFails(view.viewName, userPipe);
            dropView(view.viewName);
        });

        it("should handle multiple $addViewName stages in user pipeline", function () {
            const viewPipe = [{$addFields: {a: 1}}];
            const userPipe = [{$addViewName: {}}, {$project: {_id: 1, viewName: 1}}, {$addViewName: {}}];
            const viewName = "multi_add_viewname_view";
            testViewNameWithTemporaryView(viewName, viewPipe, userPipe, false /* shouldPrepend */);
        });
    });

    describe("Extension stage in view", function () {
        it("should not add view name when $addViewName is in view definition", function () {
            const viewPipe = [{$addViewName: {}}];
            const userPipe = [{$testFoo: {}}];
            const shouldPrepend = true; // ViewPolicy is kDefaultPrepend
            testViewNameAbsentInView("add_viewname_in_view", viewPipe, userPipe, shouldPrepend);
        });

        it("should not add view name when view has $desugarAddViewName in definition and desugaring should work in view def", function () {
            const viewPipe = [{$desugarAddViewName: {}}];
            const userPipe = [{$addFields: {a: 1}}];
            const shouldPrepend = true; // ViewPolicy is kDefaultPrepend
            testViewNameAbsentInView("desugar_add_viewname_in_view", viewPipe, userPipe, shouldPrepend);
        });

        describe("Extension stages at different positions in view definition", function () {
            it("should not add view name when $addViewName is at first position in view definition", function () {
                const viewPipe = [{$addViewName: {}}, {$addFields: {a: 1}}];
                const userPipe = [{$addFields: {a: 1}}];
                const shouldPrepend = true; // ViewPolicy is kDefaultPrepend
                testViewNameAbsentInView("add_viewname_first", viewPipe, userPipe, shouldPrepend);
            });

            it("should not add view name when $addViewName is at middle position in view definition", function () {
                const viewPipe = [{$addFields: {a: 1}}, {$addViewName: {}}, {$addFields: {a: 1}}];
                const userPipe = [{$addFields: {a: 1}}];
                const shouldPrepend = true; // ViewPolicy is kDefaultPrepend
                testViewNameAbsentInView("add_viewname_middle", viewPipe, userPipe, shouldPrepend);
            });

            it("should not add view name when $addViewName is at last position in view definition", function () {
                const viewPipe = [{$addFields: {a: 1}}, {$addViewName: {}}];
                const userPipe = [{$addFields: {a: 1}}];
                const shouldPrepend = true; // ViewPolicy is kDefaultPrepend
                testViewNameAbsentInView("add_viewname_last", viewPipe, userPipe, shouldPrepend);
            });
        });

        it("should not add view name when view has multiple extension stages in definition", function () {
            const viewPipe = [{$desugarAddViewName: {}}, {$testFoo: {}}, {$addViewName: {}}];
            const userPipe = [{$addFields: {a: 1}}];
            const shouldPrepend = true; // ViewPolicy is kDefaultPrepend
            testViewNameAbsentInView("multi_extension_view", viewPipe, userPipe, shouldPrepend);
        });
    });

    describe("Extension stage in $unionWith on view", function () {
        it("should add view name when $addViewName is in $unionWith subpipeline targeting a view", function () {
            const viewName = "union_with_view";
            const viewPipe = [{$addFields: {fromView: true}}];
            const view = createView(viewName, viewPipe);

            // $addViewName has kDoNothing policy, so the view pipeline is NOT prepended.
            const pipeline = [{$unionWith: {coll: viewName, pipeline: [{$addViewName: {}}]}}];
            const results = coll.aggregate(pipeline).toArray();

            const viewDocs = results.filter((doc) => doc.hasOwnProperty("viewName"));
            assert.gt(viewDocs.length, 0, "Expected some documents with viewName field from $unionWith");

            // Verify the view docs have the correct viewName.
            verifyViewName(viewDocs, view.viewName);

            dropView(viewName);
        });

        it("should add view name when $desugarAddViewName is in $unionWith subpipeline targeting a view", function () {
            const viewName = "union_with_desugar_view";
            const viewPipe = [{$addFields: {fromView: true}}];
            const view = createView(viewName, viewPipe);

            // $desugarAddViewName desugars to $addViewName + $doNothingViewPolicy.
            // Since it starts the pipeline, view pipeline is NOT prepended (kDoNothing policy).
            const pipeline = [{$unionWith: {coll: viewName, pipeline: [{$desugarAddViewName: {}}]}}];
            const results = coll.aggregate(pipeline).toArray();

            const viewDocs = results.filter((doc) => doc.hasOwnProperty("viewName"));
            assert.gt(viewDocs.length, 0, "Expected some documents with viewName field from $unionWith");

            // Verify the view docs have the correct viewName.
            verifyViewName(viewDocs, view.viewName);

            dropView(viewName);
        });

        it("should fail when $disallowViews is in $unionWith subpipeline targeting a view", function () {
            const viewName = "union_with_disallow_view";
            const viewPipe = [{$addFields: {fromView: true}}];
            const view = createView(viewName, viewPipe);

            // $disallowViews should fail when the $unionWith targets a view.
            const pipeline = [{$unionWith: {coll: viewName, pipeline: [{$disallowViews: {}}]}}];
            testDisallowViewsFails(collName, pipeline);

            dropView(view.viewName);
        });

        it("should fail when $disallowViews isn't first in $unionWith subpipeline targeting a view", function () {
            const viewName = "union_with_multi_policy_view";
            const viewPipe = [{$addFields: {fromView: true}}];
            const view = createView(viewName, viewPipe);

            // Even though $addViewName has kDoNothing policy, subsequent $disallowViews should fail.
            const pipeline = [{$unionWith: {coll: viewName, pipeline: [{$addViewName: {}}, {$disallowViews: {}}]}}];
            testDisallowViewsFails(collName, pipeline);

            dropView(view.viewName);
        });

        it("should not prepend view pipeline when first stage in $unionWith subpipeline has kDoNothing policy", function () {
            const viewName = "union_with_donothing_first";
            const viewPipe = [{$addFields: {fromViewPipeline: true}}];
            const view = createView(viewName, viewPipe);

            // $addViewName has kDoNothing policy, so view pipeline should NOT be prepended.
            const pipeline = [{$unionWith: {coll: viewName, pipeline: [{$addViewName: {}}]}}];
            const results = coll.aggregate(pipeline).toArray();

            const viewDocs = results.filter((doc) => doc.hasOwnProperty("viewName"));
            // Since kDoNothing doesn't prepend view pipeline, fromViewPipeline should be absent.
            viewDocs.forEach((doc) => {
                assert(
                    !doc.hasOwnProperty("fromViewPipeline"),
                    `Document should not have fromViewPipeline when view pipeline is not prepended: ${tojson(doc)}`,
                );
            });

            dropView(view.viewName);
        });

        it("should prepend view pipeline when non-first stage in $unionWith subpipeline has kDoNothing policy", function () {
            const viewName = "union_with_donothing_later";
            const viewPipe = [{$addFields: {fromViewPipeline: true}}];
            const view = createView(viewName, viewPipe);

            // $match has kDefaultPrepend policy, so view pipeline SHOULD be prepended.
            // The subsequent $addViewName doesn't affect the prepend decision.
            const pipeline = [
                {$unionWith: {coll: viewName, pipeline: [{$match: {_id: {$exists: true}}}, {$addViewName: {}}]}},
            ];
            const results = coll.aggregate(pipeline).toArray();

            const viewDocs = results.filter((doc) => doc.hasOwnProperty("viewName"));
            // Since first stage has kDefaultPrepend, view pipeline should be prepended.
            viewDocs.forEach((doc) => {
                assert(
                    doc.hasOwnProperty("fromViewPipeline"),
                    `Document should have fromViewPipeline when view pipeline is prepended: ${tojson(doc)}`,
                );
            });

            dropView(view.viewName);
        });
    });

    describe("Combined scenarios, nested views, etc.", function () {
        it("should add outer view name but not inner view name when using nested views", function () {
            testNestedViewsWithViewNameStage("_base", {$addViewName: {}});
        });

        it("should add outer view name but not inner view name when using nested views with $desugarAddViewName", function () {
            testNestedViewsWithViewNameStage("_base_desugar", {$desugarAddViewName: {}});
        });

        it("should add view name when $addViewName is both in the view definition and in the user pipeline", function () {
            const viewPipe = [{$addViewName: {}}, {$addFields: {a: 1}}];
            const userPipe = [{$addViewName: {}}];
            const viewName = "view_with_add_viewname";
            const shouldPrepend = false; // ViewPolicy is kDoNothing
            testViewNameWithTemporaryView(viewName, viewPipe, userPipe, shouldPrepend);
        });

        it("should add view name when $desugarAddViewName is both in the view definition and in the user pipeline", function () {
            const viewPipe = [{$desugarAddViewName: {}}, {$addFields: {a: 1}}];
            const userPipe = [{$desugarAddViewName: {}}];
            const viewName = "view_with_desugar";
            const shouldPrepend = false; // ViewPolicy is kDoNothing
            testViewNameWithTemporaryView(viewName, viewPipe, userPipe, shouldPrepend);
        });
    });

    describe("$disallowViews in nested view contexts", function () {
        it("should fail when $disallowViews used on nested view (view on view)", function () {
            const baseViewName = "nested_base_disallow";
            const baseViewPipe = [{$addFields: {fromBase: true}}];
            assert.commandWorked(db.createView(baseViewName, collName, baseViewPipe));

            const topViewName = "nested_top_disallow";
            const topViewPipe = [{$addFields: {fromTop: true}}];
            assert.commandWorked(db.createView(topViewName, baseViewName, topViewPipe));

            const pipeline = [{$disallowViews: {}}];
            testDisallowViewsFails(topViewName, pipeline);

            dropView(topViewName);
            dropView(baseViewName);
        });

        it("should fail when $disallowViews used in $unionWith targeting nested view", function () {
            const baseViewName = "nested_base_unionwith_disallow";
            const baseViewPipe = [{$addFields: {fromBase: true}}];
            assert.commandWorked(db.createView(baseViewName, collName, baseViewPipe));

            const topViewName = "nested_top_unionwith_disallow";
            const topViewPipe = [{$addFields: {fromTop: true}}];
            assert.commandWorked(db.createView(topViewName, baseViewName, topViewPipe));

            const pipeline = [{$unionWith: {coll: topViewName, pipeline: [{$disallowViews: {}}]}}];
            testDisallowViewsFails(collName, pipeline);

            dropView(topViewName);
            dropView(baseViewName);
        });
    });
});
