/**
 * Integration tests for $_internalDocumentResultsAndMetadata feeding terminal write stages
 * ($out and $merge).
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 *  requires_fcv_90,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {
    expandPerShard,
    getDrmShardInfo,
    kSimpleExpectedMeta,
    setupDrmCollection,
} from "jstests/extensions/libs/document_results_and_metadata_utils.js";

describe("$_internalDocumentResultsAndMetadata with terminal stages", function () {
    let coll;
    let target;
    let nShards;
    let isSharded;

    before(function () {
        coll = db[jsTestName()];
        target = db[jsTestName() + "_target"];
        setupDrmCollection(db, coll);
        ({nShards, isSharded} = getDrmShardInfo(db, coll));
    });

    beforeEach(function () {
        // Each test owns the target collection.
        assertDropCollection(db, target.getName());
    });

    it("writes DRM docs with $$SEARCH_META to a target via $out", function () {
        const numDocs = 3;
        assert.commandWorked(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {$extensionMultiStream: {numDocs, meta: kSimpleExpectedMeta}},
                    {$project: {_id: 0, name: 1, meta: "$$SEARCH_META"}},
                    {$out: target.getName()},
                ],
                cursor: {},
            }),
        );

        const written = target.find({}, {_id: 0}).toArray();
        const perShardDocs = [
            {name: "doc_0", meta: kSimpleExpectedMeta},
            {name: "doc_1", meta: kSimpleExpectedMeta},
            {name: "doc_2", meta: kSimpleExpectedMeta},
        ];
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(written, expected, {written, nShards});
    });

    it("merges DRM docs with $$SEARCH_META into a target via $merge", function () {
        assert.commandWorked(target.createIndex({name: 1}, {unique: true}));

        const numDocs = 3;
        assert.commandWorked(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {$extensionMultiStream: {numDocs, meta: kSimpleExpectedMeta}},
                    {$project: {_id: 0, name: 1, meta: "$$SEARCH_META"}},
                    {
                        $merge: {
                            into: target.getName(),
                            on: "name",
                            whenMatched: "replace",
                            whenNotMatched: "insert",
                        },
                    },
                ],
                cursor: {},
            }),
        );

        // The DRM source produces docs named doc_0…doc_{numDocs-1}. In sharded mode each shard
        // emits the same set; $merge collapses them via the unique `name` key.
        const written = target.find({}, {_id: 0}).toArray();
        const expected = [
            {name: "doc_0", meta: kSimpleExpectedMeta},
            {name: "doc_1", meta: kSimpleExpectedMeta},
            {name: "doc_2", meta: kSimpleExpectedMeta},
        ];
        assert.sameMembers(written, expected, {written});
    });

    it("[sharded] $out target reflects pipeline-split DRM output", function () {
        if (!isSharded) return;
        const numDocs = 4;
        assert.commandWorked(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {$extensionMultiStream: {numDocs, meta: kSimpleExpectedMeta}},
                    {$project: {_id: 0, name: 1, score: 1, meta: "$$SEARCH_META"}},
                    {$out: target.getName()},
                ],
                cursor: {},
            }),
        );

        const written = target.find({}, {_id: 0}).toArray();
        const perShardDocs = [
            {name: "doc_0", score: 4, meta: kSimpleExpectedMeta},
            {name: "doc_1", score: 3, meta: kSimpleExpectedMeta},
            {name: "doc_2", score: 2, meta: kSimpleExpectedMeta},
            {name: "doc_3", score: 1, meta: kSimpleExpectedMeta},
        ];
        const expected = expandPerShard(nShards, perShardDocs);
        assert.sameMembers(written, expected, {written, nShards});
    });
});
