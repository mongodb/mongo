/**
 * Tests interactions between $_internalDocumentResultsAndMetadata (via $extensionMultiStream) and
 * a $searchMeta source stage.
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 *  featureFlagExtensionsInsideHybridSearch,
 * ]
 */
import {
    checkPlatformCompatibleWithExtensions,
    withExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

withExtensions(
    {"libmulti_stream_mongo_extension.so": {}, "libsearch_extension.so": {}},
    (conn) => {
        const coll = conn.getDB("test")[jsTestName()];
        assert.commandWorked(
            conn.getDB("admin").runCommand({setParameter: 1, featureFlagSearchExtension: true}),
        );

        {
            // $_internalDocumentResultsAndMetadata and $searchMeta cannot coexist; both are kFirst
            // source stages.
            assert.commandFailedWithCode(
                coll.runCommand("aggregate", {
                    pipeline: [
                        {$extensionMultiStream: {numDocs: 1, meta: {count: {lowerBound: 42}}}},
                        {$searchMeta: {}},
                    ],
                    cursor: {},
                }),
                40602,
            );
        }

        const meta = {count: {lowerBound: 42}};

        {
            // $extensionMultiStream in the outer pipeline and $searchMeta in a $unionWith subpipeline
            // are at different pipeline levels, so each can be a kFirst source stage without conflict.
            // Two docs from $extensionMultiStream plus one metadata doc from $searchMeta.
            const results = coll
                .aggregate([
                    {$extensionMultiStream: {numDocs: 2, meta}},
                    {$unionWith: {coll: coll.getName(), pipeline: [{$searchMeta: {}}]}},
                ])
                .toArray();
            assert.eq(results.length, 3, {results});
        }

        {
            // $$SEARCH_META cannot be accessed after a stage with a sub-pipeline.
            assert.commandFailedWithCode(
                coll.runCommand("aggregate", {
                    pipeline: [
                        {$extensionMultiStream: {numDocs: 2, meta}},
                        {$unionWith: {coll: coll.getName(), pipeline: [{$searchMeta: {}}]}},
                        {$project: {_id: 0, searchMeta: "$$SEARCH_META"}},
                    ],
                    cursor: {},
                }),
                6347901,
            );
        }
    },
    ["standalone"],
);
