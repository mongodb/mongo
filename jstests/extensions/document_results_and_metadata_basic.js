/**
 * Integration tests for $_internalDocumentResultsAndMetadata core document-only behavior.
 *
 * Uses $extensionMultiStream (no "meta" field) to exercise the single-stream path through the
 * Exchange router: the extension desugars to $_internalDocumentResultsAndMetadata with no metadata
 * spec, wrapping $_multiStreamSource as the source.
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 *  requires_fcv_90,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {before, describe, it} from "jstests/libs/mochalite.js";
import {
    expandEachPerShard,
    expandPerShard,
    getDrmShardInfo,
} from "jstests/extensions/libs/document_results_and_metadata_utils.js";

describe("$_internalDocumentResultsAndMetadata basic document-only behavior", function () {
    let coll;
    let nShards;

    before(function () {
        coll = db[jsTestName()];
        assertDropCollection(db, coll.getName());
        assert.commandWorked(coll.insertOne({placeholder: true}));
        ({nShards} = getDrmShardInfo(db, coll));
    });

    it("returns all docs from every shard", function () {
        const result = coll.aggregate([{$extensionMultiStream: {numDocs: 5}}]).toArray();
        const perShardDocs = [
            {_id: 0, score: 5, name: "doc_0"},
            {_id: 1, score: 4, name: "doc_1"},
            {_id: 2, score: 3, name: "doc_2"},
            {_id: 3, score: 2, name: "doc_3"},
            {_id: 4, score: 1, name: "doc_4"},
        ];
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("returns empty result set when source produces 0 documents", function () {
        const result = coll.aggregate([{$extensionMultiStream: {numDocs: 0}}]).toArray();
        assert.eq(result, [], {result});
    });

    it("filters documents with a downstream $match", function () {
        const result = coll
            .aggregate([{$extensionMultiStream: {numDocs: 5}}, {$match: {score: {$gt: 3}}}])
            .toArray();
        const perShardDocs = [
            {_id: 0, score: 5, name: "doc_0"},
            {_id: 1, score: 4, name: "doc_1"},
        ];
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("projects fields correctly with a downstream $project", function () {
        const result = coll
            .aggregate([{$extensionMultiStream: {numDocs: 3}}, {$project: {name: 1, score: 1}}])
            .toArray();
        const perShardDocs = [
            {_id: 0, score: 3, name: "doc_0"},
            {_id: 1, score: 2, name: "doc_1"},
            {_id: 2, score: 1, name: "doc_2"},
        ];
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("re-sorts documents with a downstream $sort", function () {
        const result = coll
            .aggregate([{$extensionMultiStream: {numDocs: 5}}, {$sort: {name: 1}}])
            .toArray();
        const perShardDocs = [
            {_id: 0, score: 5, name: "doc_0"},
            {_id: 1, score: 4, name: "doc_1"},
            {_id: 2, score: 3, name: "doc_2"},
            {_id: 3, score: 2, name: "doc_3"},
            {_id: 4, score: 1, name: "doc_4"},
        ];
        // After $sort, every per-shard doc appears nShards times.
        const expected = expandEachPerShard(nShards, perShardDocs);
        assert.eq(result, expected, {result, nShards});
    });

    it("returns all documents across getMore batches when source produces >101 docs", function () {
        const numDocs = 150;
        const result = coll.aggregate([{$extensionMultiStream: {numDocs}}]).toArray();
        const perShardDocs = Array.from({length: numDocs}, (_, i) => ({
            _id: i,
            score: numDocs - i,
            name: `doc_${i}`,
        }));
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("returns all 500 docs across dynamic exchange batches", function () {
        const numDocs = 500;
        const result = coll.aggregate([{$extensionMultiStream: {numDocs}}]).toArray();
        const perShardDocs = Array.from({length: numDocs}, (_, i) => ({
            _id: i,
            score: numDocs - i,
            name: `doc_${i}`,
        }));
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("$limit through a batched exchange returns exactly N docs", function () {
        const result = coll
            .aggregate([{$extensionMultiStream: {numDocs: 200}}, {$limit: 10}])
            .toArray();
        assert.eq(result.length, 10, {result});
    });

    it("returns all docs when document stream exceeds the Exchange buffer", function () {
        const numDocs = 40;
        const docPad = 512 * 1024;
        const result = coll.aggregate([{$extensionMultiStream: {numDocs, docPad}}]).toArray();
        const perShardDocs = Array.from({length: numDocs}, (_, i) => ({
            _id: i,
            score: numDocs - i,
            name: `doc_${i}`,
        }));
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(
            result.map((d) => ({_id: d._id, score: d.score, name: d.name})),
            expected,
            {nShards},
        );
    });

    it("preserves user-owned _streamType field in output documents", function () {
        const result = coll
            .aggregate([{$extensionMultiStream: {numDocs: 3, addStreamTypeField: true}}])
            .toArray();
        const perShardDocs = [
            {_id: 0, score: 3, name: "doc_0", _streamType: -1},
            {_id: 1, score: 2, name: "doc_1", _streamType: -1},
            {_id: 2, score: 1, name: "doc_2", _streamType: -1},
        ];
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(result, expected, {result, nShards});
    });
});
