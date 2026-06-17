/**
 * Error-case integration tests for $_internalDocumentResultsAndMetadata.
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 *  featureFlagExtensionsInsideHybridSearch,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

const expectedMeta = {count: {lowerBound: 42}};

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
            {$extensionMultiStream: {numDocs: 3, meta: expectedMeta, numMeta: 0}},
            {$project: {name: 1, meta: "$$SEARCH_META"}},
        ],
        code: 625296,
    },
    {
        // numMeta:2 makes the source emit 2 kMetaResult docs; $setVariableFromSubPipeline
        // expects exactly one. A passthrough mergePipeline preserves the multi-doc condition
        // in sharded topologies (the default $group-based merge would collapse the two docs
        // into one and mask the error after merge).
        name: "errors when source produces multiple metadata documents",
        pipeline: [
            {
                $extensionMultiStream: {
                    numDocs: 1,
                    meta: expectedMeta,
                    numMeta: 2,
                    mergePipeline: [{$match: {}}],
                },
            },
            {$project: {meta: "$$SEARCH_META"}},
        ],
        code: 625297,
    },
];

describe("$_internalDocumentResultsAndMetadata error cases", function () {
    let coll;

    before(function () {
        coll = db[jsTestName()];
        assertDropCollection(db, coll.getName());
        assert.commandWorked(coll.insertOne({placeholder: true}));
    });

    for (const {name, pipeline, code} of errorCases) {
        it(name, function () {
            assert.commandFailedWithCode(
                db.runCommand({aggregate: coll.getName(), pipeline, cursor: {}}),
                code,
            );
        });
    }
});
