/**
 * Integration tests for $_internalDocumentResultsAndMetadata pipeline splitting in sharded
 * topologies. Verifies which downstream stages move to the shard side vs. stay on the router by
 * inspecting the explain output's splitPipeline. Source DRM is configured with metadata so
 * $$SEARCH_META scoping affects canMovePast decisions.
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 *  featureFlagExtensionsInsideHybridSearch,
 *  requires_sharding,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {before, describe, it} from "jstests/libs/mochalite.js";
import {kSimpleExpectedMeta} from "jstests/extensions/libs/document_results_and_metadata_utils.js";

function stageNames(part) {
    return (part || []).map((s) => Object.keys(s)[0]);
}

describe("$_internalDocumentResultsAndMetadata pipeline split", function () {
    let coll;

    before(function () {
        coll = db[jsTestName()];
        assertDropCollection(db, coll.getName());
        assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
        assert.commandWorked(
            db.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}),
        );
    });

    function getSplit(pipeline) {
        return coll.explain().aggregate(pipeline).splitPipeline;
    }

    it("$match without $$SEARCH_META reference moves to router", function () {
        const split = getSplit([
            {$extensionMultiStream: {numDocs: 5, meta: kSimpleExpectedMeta}},
            {$match: {score: {$gt: 3}}},
        ]);
        assert(split, "expected a split pipeline");
        assert.contains("$match", stageNames(split.mergerPart), {split});
        assert(!stageNames(split.shardsPart).includes("$match"), {split});
    });

    it("$project referencing $$SEARCH_META stays on router", function () {
        // DRM uses returnCursor:true in sharded mode so $$SEARCH_META is only bound at the
        // router (via the metadata merge pipeline). Downstream stages that reference $$SEARCH_META
        // cannot run on shards and must stay on the router side of the split.
        const split = getSplit([
            {$extensionMultiStream: {numDocs: 5, meta: kSimpleExpectedMeta}},
            {$project: {name: 1, meta: "$$SEARCH_META"}},
        ]);
        assert(split, "expected a split pipeline");
        assert.contains("$project", stageNames(split.mergerPart), {split});
        assert(!stageNames(split.shardsPart).includes("$project"), {split});
    });

    it("$sort moves to router for global ordering", function () {
        const split = getSplit([
            {$extensionMultiStream: {numDocs: 5, meta: kSimpleExpectedMeta}},
            {$sort: {name: 1}},
        ]);
        assert(split, "expected a split pipeline");
        assert.contains("$sort", stageNames(split.mergerPart), {split});
    });

    it("$limit pushes down to shards", function () {
        const split = getSplit([
            {$extensionMultiStream: {numDocs: 5, meta: kSimpleExpectedMeta}},
            {$limit: 5},
        ]);
        assert(split, "expected a split pipeline");
        assert.contains("$limit", stageNames(split.shardsPart), {split});
    });

    it("$project without $$SEARCH_META reference moves to router with shard-side dependency projection", function () {
        const split = getSplit([
            {$extensionMultiStream: {numDocs: 5, meta: kSimpleExpectedMeta}},
            {$project: {name: 1}},
        ]);
        assert(split, "expected a split pipeline");
        assert.contains("$project", stageNames(split.mergerPart), {split});
        assert.contains("$project", stageNames(split.shardsPart), {split});
    });
});
