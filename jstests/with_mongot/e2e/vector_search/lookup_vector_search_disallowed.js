/**
 * Tests that $vectorSearch inside a $lookup subpipeline is rejected with error 51047 while
 * featureFlagExtensionsInsideHybridSearch is disabled, both when the foreign namespace is a
 * collection and when it is a view.
 *
 * TODO SERVER-121094 Remove this test when the feature flag is removed.
 *
 * @tags: [
 *   # Specifically testing that $vectorSearch in $lookup is rejected when
 *   # featureFlagExtensionsInsideHybridSearch is not enabled; the flag-on behavior is covered by
 *   # lookup_vector_search.js.
 *   featureFlagExtensionsInsideHybridSearch_incompatible,
 * ]
 */

const collName = jsTestName();

const coll = db.getCollection(collName);
assert.commandWorked(coll.insert({_id: 0}));

function makeVectorSearchStage() {
    return {$vectorSearch: {queryVector: [], path: "x", numCandidates: 1, limit: 1}};
}

function runPipeline(pipeline) {
    return db.runCommand({aggregate: collName, pipeline, cursor: {}});
}

try {
    // $vectorSearch in a $lookup subpipeline targeting a collection is rejected.
    assert.commandFailedWithCode(
        runPipeline([
            {$lookup: {from: collName, pipeline: [makeVectorSearchStage()], as: "lookup1"}},
        ]),
        51047,
    );

    // $vectorSearch in a $lookup subpipeline targeting a view is also rejected.
    const viewName = jsTestName() + "_view";
    assert.commandWorked(db.createView(viewName, collName, []));
    try {
        assert.commandFailedWithCode(
            runPipeline([
                {$lookup: {from: viewName, pipeline: [makeVectorSearchStage()], as: "lookup_view"}},
            ]),
            51047,
        );
    } finally {
        db.getCollection(viewName).drop();
    }
} finally {
    coll.drop();
}
