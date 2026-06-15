/**
 * Integration tests for $_internalDocumentResultsAndMetadata core document-only behavior.
 *
 * Uses $extensionMultiStream (no "meta" field) to exercise the single-stream path through the
 * Exchange router: the extension desugars to $_internalDocumentResultsAndMetadata with no metadata
 * spec, wrapping $_multiStreamSource as the source.
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 *  featureFlagExtensionsInsideHybridSearch,
 *  # TODO SERVER-128454: drop when sharded-aware scenarios are added.
 *  assumes_unsharded_collection,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

describe("$_internalDocumentResultsAndMetadata basic document-only behavior", function () {
    let coll;

    before(function () {
        coll = db[jsTestName()];
        assertDropCollection(db, coll.getName());
        assert.commandWorked(coll.insertOne({placeholder: true}));
    });

    it("returns 5 documents in source-defined order", function () {
        const result = coll.aggregate([{$extensionMultiStream: {numDocs: 5}}]).toArray();
        assert.docEq(
            [
                {_id: 0, score: 5, name: "doc_0"},
                {_id: 1, score: 4, name: "doc_1"},
                {_id: 2, score: 3, name: "doc_2"},
                {_id: 3, score: 2, name: "doc_3"},
                {_id: 4, score: 1, name: "doc_4"},
            ],
            result,
            {result},
        );
    });

    it("returns empty result set when source produces 0 documents", function () {
        const result = coll.aggregate([{$extensionMultiStream: {numDocs: 0}}]).toArray();
        assert.eq(result.length, 0, {result});
    });

    it("filters documents with a downstream $match", function () {
        const result = coll
            .aggregate([{$extensionMultiStream: {numDocs: 5}}, {$match: {score: {$gt: 3}}}])
            .toArray();
        assert.docEq(
            [
                {_id: 0, score: 5, name: "doc_0"},
                {_id: 1, score: 4, name: "doc_1"},
            ],
            result,
            {result},
        );
    });

    it("projects fields correctly with a downstream $project", function () {
        const result = coll
            .aggregate([{$extensionMultiStream: {numDocs: 3}}, {$project: {name: 1, score: 1}}])
            .toArray();
        assert.docEq(
            [
                {_id: 0, score: 3, name: "doc_0"},
                {_id: 1, score: 2, name: "doc_1"},
                {_id: 2, score: 1, name: "doc_2"},
            ],
            result,
            {result},
        );
    });

    it("re-sorts documents with a downstream $sort", function () {
        const result = coll
            .aggregate([{$extensionMultiStream: {numDocs: 5}}, {$sort: {name: 1}}])
            .toArray();
        assert.eq(result.length, 5, {result});
        for (let i = 0; i < result.length - 1; i++) {
            assert.lte(result[i].name, result[i + 1].name, {result});
        }
    });

    it("returns all documents across getMore batches when source produces >101 docs", function () {
        const numDocs = 150;
        const result = coll.aggregate([{$extensionMultiStream: {numDocs}}]).toArray();
        assert.eq(result.length, numDocs, {result});
        for (const [i, doc] of result.entries()) {
            assert.eq(doc._id, i, {result});
        }
    });

    it("preserves user-owned _streamType field in output documents", function () {
        const result = coll
            .aggregate([{$extensionMultiStream: {numDocs: 3, addStreamTypeField: true}}])
            .toArray();
        assert.docEq(
            [
                {_id: 0, score: 3, name: "doc_0", _streamType: -1},
                {_id: 1, score: 2, name: "doc_1", _streamType: -1},
                {_id: 2, score: 1, name: "doc_2", _streamType: -1},
            ],
            result,
            {result},
        );
    });
});
