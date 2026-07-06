/**
 * Tests that $vectorSearch inside a $lookup subpipeline is rejected with error 51047 while
 * featureFlagExtensionsInsideHybridSearch is disabled, both when the foreign namespace is a
 * collection and when it is a view.
 *
 * TODO SERVER-121094 Remove this test when the feature flag is removed.
 *
 * @tags: [
 *   assumes_stable_shard_list,
 *   requires_fcv_90,
 * ]
 */

import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
assert.commandWorked(coll.insert({_id: 0}));

function makeVectorSearchStage() {
    return {$vectorSearch: {queryVector: [], path: "x", numCandidates: 1, limit: 1}};
}

function runPipeline(pipeline) {
    return db.runCommand({aggregate: collName, pipeline, cursor: {}});
}

runWithParamsAllNonConfigNodes(db, {featureFlagExtensionsInsideHybridSearch: false}, () => {
    try {
        // $vectorSearch in a $lookup subpipeline targeting a collection is rejected.
        assert.commandFailedWithCode(
            runPipeline([
                {
                    $lookup: {
                        from: collName,
                        pipeline: [makeVectorSearchStage()],
                        as: "lookup1",
                    },
                },
            ]),
            51047,
        );

        // $vectorSearch in a $lookup subpipeline targeting a view is also rejected.
        const viewName = jsTestName() + "_view";
        assert.commandWorked(db.createView(viewName, collName, []));
        try {
            assert.commandFailedWithCode(
                runPipeline([
                    {
                        $lookup: {
                            from: viewName,
                            pipeline: [makeVectorSearchStage()],
                            as: "lookup_view",
                        },
                    },
                ]),
                51047,
            );
        } finally {
            // Drop the view via runCommand rather than db.getCollection(viewName).drop(): in the
            // implicitly_shard_accessed_collections passthrough, DBCollection.prototype.drop
            // re-shards the namespace after dropping, which would re-create viewName as a sharded
            // collection and make a subsequent createView (e.g. on a repeated test run) fail with
            // NamespaceExists.
            assert.commandWorked(db.runCommand({drop: viewName}));
        }
    } finally {
        coll.drop();
    }
});
