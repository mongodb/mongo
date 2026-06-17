/**
 * Integration tests for $_internalDocumentResultsAndMetadata interacting with $lookup.
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 *  featureFlagExtensionsInsideHybridSearch,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

const expectedMeta = {count: {lowerBound: 42}};

describe("$_internalDocumentResultsAndMetadata with $lookup", function () {
    let coll;
    let foreignColl;
    let nShards;

    before(function () {
        coll = db[jsTestName()];
        foreignColl = db[jsTestName() + "_foreign"];
        assertDropCollection(db, coll.getName());
        assertDropCollection(db, foreignColl.getName());
        if (FixtureHelpers.isMongos(db)) {
            assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
            assert.commandWorked(
                db.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}),
            );
        }
        assert.commandWorked(coll.insertOne({_id: 0}));
        assert.commandWorked(
            foreignColl.insertMany([
                {_id: "f0", name: "doc_0", extra: "x0"},
                {_id: "f1", name: "doc_1", extra: "x1"},
                {_id: "f2", name: "doc_2", extra: "x2"},
            ]),
        );
        nShards = FixtureHelpers.numberOfShardsForCollection(coll);
    });

    it("attaches lookup results while preserving $$SEARCH_META projected onto each doc", function () {
        const numDocs = 3;
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs, meta: expectedMeta}},
                {$project: {_id: 0, name: 1, meta: "$$SEARCH_META"}},
                {
                    $lookup: {
                        from: foreignColl.getName(),
                        localField: "name",
                        foreignField: "name",
                        as: "joined",
                    },
                },
            ])
            .toArray();

        const perShardDocs = [
            {name: "doc_0", meta: expectedMeta, joined: [{_id: "f0", name: "doc_0", extra: "x0"}]},
            {name: "doc_1", meta: expectedMeta, joined: [{_id: "f1", name: "doc_1", extra: "x1"}]},
            {name: "doc_2", meta: expectedMeta, joined: [{_id: "f2", name: "doc_2", extra: "x2"}]},
        ];
        const expected = Array(nShards).fill(perShardDocs).flat();
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("forwards $$SEARCH_META through a $lookup let-binding", function () {
        const numDocs = 2;
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs, meta: expectedMeta}},
                {$project: {_id: 0, name: 1}},
                {
                    $lookup: {
                        from: foreignColl.getName(),
                        let: {meta: "$$SEARCH_META"},
                        pipeline: [{$limit: 1}, {$project: {_id: 0, outerMeta: "$$meta"}}],
                        as: "joined",
                    },
                },
            ])
            .toArray();

        const perShardDocs = [
            {name: "doc_0", joined: [{outerMeta: expectedMeta}]},
            {name: "doc_1", joined: [{outerMeta: expectedMeta}]},
        ];
        const expected = Array(nShards).fill(perShardDocs).flat();
        assert.sameMembers(result, expected, {result, nShards});
    });

    it("runs an extension DRM source inside a $lookup subpipeline and forwards $$SEARCH_META", function () {
        const numDocs = 2;
        const result = coll
            .aggregate([
                {
                    $lookup: {
                        from: foreignColl.getName(),
                        pipeline: [
                            {$extensionMultiStream: {numDocs, meta: expectedMeta}},
                            {$project: {_id: 0, name: 1, meta: "$$SEARCH_META"}},
                        ],
                        as: "joined",
                    },
                },
            ])
            .toArray();

        assert.eq(result.length, 1, {result});
        assert.sameMembers(
            result[0].joined,
            [
                {name: "doc_0", meta: expectedMeta},
                {name: "doc_1", meta: expectedMeta},
            ],
            {result},
        );
    });

    it("rejects $$SEARCH_META reference in top-level after a $lookup that had no DRM source", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {$extensionMultiStream: {numDocs: 1, meta: expectedMeta}},
                    {
                        $lookup: {
                            from: foreignColl.getName(),
                            localField: "name",
                            foreignField: "name",
                            as: "joined",
                        },
                    },
                    {$project: {meta: "$$SEARCH_META"}},
                ],
                cursor: {},
            }),
            6347901,
        );
    });
});
