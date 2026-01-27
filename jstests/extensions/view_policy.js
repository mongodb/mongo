/**
 * This test verifies that the $addViewName extension stage correctly passes the view name
 * through the extension boundary (AstNode -> LogicalStage -> ExecAggStage) and adds
 * it as a field in output documents.
 *
 * This test also verifies that the $disallowViews extension stage correctly uasserts when
 * used in a view context.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insertMany([{x: 1}, {x: 2}, {x: 3}]));

function createView(viewName, viewPipeline) {
    assert.commandWorked(db.createView(viewName, jsTestName(), viewPipeline));
    return {
        view: db[viewName],
        viewName,
        viewNss: `${db.getName()}.${viewName}`,
    };
}

function dropView(viewName) {
    assert.commandWorked(db.runCommand({drop: viewName}));
}

function verifyViewName(results, expectedViewNss) {
    results.forEach((doc, index) => {
        assert(doc.hasOwnProperty("viewName"), `Document ${index} should have viewName field: ${tojson(doc)}`);
        assert.eq(
            doc.viewName,
            expectedViewNss,
            `Document ${index} should have viewName field equal to "${expectedViewNss}": ${tojson(doc)}`,
        );
    });
}

function testViewNameOnView(view, pipeline, expectedViewNss) {
    const result = view.view.aggregate(pipeline).toArray();
    verifyViewName(result, expectedViewNss);
}

function testViewNameWithTemporaryView(viewName, viewPipeline, pipeline, expectedViewNss) {
    const testView = createView(viewName, viewPipeline);
    testViewNameOnView(testView, pipeline, expectedViewNss);
    dropView(testView.viewName);
}

const viewPipeline = [{$sort: {_id: 1}}];
const view = createView("test_view", viewPipeline);

// Tests that the view name is present at execution time.
testViewNameOnView(view, [{$addViewName: {}}], view.viewNss);

// Tests that the view name is present if the view is on an extension stage.
testViewNameWithTemporaryView("foo_view", [{$testFoo: {}}], [{$addViewName: {}}], `${db.getName()}.foo_view`);

// Tests view name is empty if the $addViewName stage is in the view.
testViewNameWithTemporaryView("add_foo_view", [{$addViewName: {}}], [{$testFoo: {}}], "");

// Tests that view name is added if the $addViewName stage is later on in the pipeline.
testViewNameOnView(view, [{$sort: {_id: -1}}, {$addViewName: {}}], view.viewNss);

// Tests that the view name is added if the $addViewName is desugared into.
testViewNameOnView(view, [{$sort: {_id: 1}}, {$desugarAddViewName: {}}], view.viewNss);

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

// Test that $disallowViews stage uasserts when run on a view.
testDisallowViewsFails(view.viewName, [{$disallowViews: {}}]);

// Test that $disallowViews stage uasserts when run on a view, even when not the first stage in a pipeline.
testDisallowViewsFails(view.viewName, [{$project: {_id: 0}}, {$disallowViews: {}}]);

{
    // Test that $disallowViews stage works correctly on a regular collection.
    coll.aggregate([{$disallowViews: {}}]);

    // Test that $disallowViews stage can be used inside a view pipeline
    const viewWithDisallowViews = createView("new_view", [{$disallowViews: {}}]);
    viewWithDisallowViews.view.aggregate([{$project: {_id: 1}}]);
    dropView(viewWithDisallowViews.viewName);
}
