/**
 * Error-case integration tests for $_internalDocumentResultsAndMetadata.
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 *  requires_fcv_90,
 * ]
 */
import {before, describe, it} from "jstests/libs/mochalite.js";
import {
    getDrmShardInfo,
    kSimpleExpectedMeta,
    setupDrmCollection,
} from "jstests/extensions/libs/document_results_and_metadata_utils.js";

const errorCases = [
    {
        name: "rejects $_internalDocumentResultsAndMetadata not at first position",
        pipeline: [{$match: {_id: 1}}, {$extensionMultiStream: {numDocs: 3}}],
        code: 40602,
    },
    {
        name: "rejects two $_internalDocumentResultsAndMetadata stages at the same pipeline level",
        pipeline: [{$extensionMultiStream: {numDocs: 3}}, {$extensionMultiStream: {numDocs: 3}}],
        code: 40602,
    },
    {
        name: "rejects $_internalDocumentResultsAndMetadata inside a $facet subpipeline",
        pipeline: [{$facet: {stream: [{$extensionMultiStream: {numDocs: 3}}]}}],
        code: 40600,
    },
    {
        name: "errors on $$SEARCH_META reference when no $_internalDocumentResultsAndMetadata in pipeline",
        pipeline: [{$project: {meta: "$$SEARCH_META"}}],
        code: 6347902,
    },
    {
        // numMeta:0 configures DRM with metadata but makes the source emit no metadata docs.
        // $setVariableFromSubPipeline requires exactly one metadata document.
        name: "errors when source produces no metadata documents",
        pipeline: [
            {$extensionMultiStream: {numDocs: 3, meta: kSimpleExpectedMeta, numMeta: 0}},
            {$project: {name: 1, meta: "$$SEARCH_META"}},
        ],
        code: 625296,
    },
];

describe("$_internalDocumentResultsAndMetadata error cases", function () {
    let coll;
    let nShards;

    before(function () {
        coll = db[jsTestName()];
        setupDrmCollection(db, coll);
        ({nShards} = getDrmShardInfo(db, coll));
    });

    for (const {name, pipeline, code} of errorCases) {
        it(name, function () {
            assert.commandFailedWithCode(
                db.runCommand({aggregate: coll.getName(), pipeline, cursor: {}}),
                code,
            );
        });
    }

    it("[sharded] errors when the merged metadata has multiple documents", function () {
        if (nShards < 2) return;
        const pipeline = [
            {
                $extensionMultiStream: {
                    numDocs: 1,
                    meta: kSimpleExpectedMeta,
                    numMeta: 2,
                    mergePipeline: [{$match: {}}],
                },
            },
            {$project: {_id: 0, meta: "$$SEARCH_META"}},
        ];
        assert.commandFailedWithCode(
            db.runCommand({aggregate: coll.getName(), pipeline, cursor: {}}),
            625297,
        );
    });
});
