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

describe("$_internalDocumentResultsAndMetadata error cases", function () {
    let coll;

    before(function () {
        coll = db[jsTestName()];
        assertDropCollection(db, coll.getName());
        assert.commandWorked(coll.insertOne({placeholder: true}));
    });

    it("rejects $_internalDocumentResultsAndMetadata not at first position", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [{$match: {_id: 1}}, {$extensionMultiStream: {numDocs: 3}}],
                cursor: {},
            }),
            40602,
        );
    });

    it("rejects two $_internalDocumentResultsAndMetadata stages at the same pipeline level", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {$extensionMultiStream: {numDocs: 3}},
                    {$extensionMultiStream: {numDocs: 3}},
                ],
                cursor: {},
            }),
            40602,
        );
    });

    it("rejects $_internalDocumentResultsAndMetadata inside a $facet subpipeline", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [{$facet: {stream: [{$extensionMultiStream: {numDocs: 3}}]}}],
                cursor: {},
            }),
            40600,
        );
    });

    it("errors on $$SEARCH_META reference when no $_internalDocumentResultsAndMetadata in pipeline", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [{$project: {meta: "$$SEARCH_META"}}],
                cursor: {},
            }),
            6347902,
        );
    });

    it("errors when source produces no metadata documents", function () {
        // numMeta:0 configures DRM with metadata but makes the source emit no metadata docs.
        // $setVariableFromSubPipeline requires exactly one metadata document.
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {$extensionMultiStream: {numDocs: 3, meta: expectedMeta, numMeta: 0}},
                    {$project: {name: 1, meta: "$$SEARCH_META"}},
                ],
                cursor: {},
            }),
            625296,
        );
    });

    it("errors when source produces multiple metadata documents", function () {
        // numMeta:2 makes the source emit 2 kMetaResult docs; $setVariableFromSubPipeline
        // expects exactly one.
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {$extensionMultiStream: {numDocs: 1, meta: expectedMeta, numMeta: 2}},
                    {$project: {meta: "$$SEARCH_META"}},
                ],
                cursor: {},
            }),
            625297,
        );
    });
});
