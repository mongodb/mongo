/**
 * Integration tests for $_internalDocumentResultsAndMetadata with metadata binding ($$SEARCH_META).
 *
 * Uses $extensionMultiStream with a "meta" field to exercise the two-stream path through the
 * Exchange router and $$SEARCH_META variable binding.
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 *  featureFlagExtensionsInsideHybridSearch,
 *  # TODO SERVER-128454: Add sharded scenarios and drop assumes_unsharded_collection.
 *  assumes_unsharded_collection,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

const expectedMeta = {
    count: {lowerBound: 42},
    facets: {
        category: [
            {value: "books", count: 10},
            {value: "movies", count: 32},
        ],
    },
};

describe("$_internalDocumentResultsAndMetadata with $$SEARCH_META binding", function () {
    let coll;

    before(function () {
        coll = db[jsTestName()];
        assertDropCollection(db, coll.getName());
        assert.commandWorked(coll.insertOne({placeholder: true}));
    });

    it("returns docs and correct faceted metadata via $$SEARCH_META", function () {
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 3, meta: expectedMeta}},
                {$project: {name: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        assert.eq(result.length, 3, {result});
        for (const doc of result) {
            assert(doc.hasOwnProperty("name"), "missing name", {doc});
            assert.docEq(doc.meta, expectedMeta, {doc});
        }
    });

    it("allows accessing nested $$SEARCH_META fields in $project", function () {
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 3, meta: expectedMeta}},
                {
                    $project: {
                        name: 1,
                        lowerBound: "$$SEARCH_META.count.lowerBound",
                        categories: "$$SEARCH_META.facets.category",
                    },
                },
            ])
            .toArray();
        assert.eq(result.length, 3, {result});
        for (const doc of result) {
            assert.eq(doc.lowerBound, 42, {doc});
            assert.eq(doc.categories.length, 2, {doc});
            assert.eq(doc.categories[0].value, "books", {doc});
            assert.eq(doc.categories[1].value, "movies", {doc});
        }
    });

    it("filters with $match on $$SEARCH_META field and projects metadata", function () {
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 3, meta: expectedMeta}},
                {$match: {$expr: {$gt: ["$$SEARCH_META.count.lowerBound", 0]}}},
                {$project: {name: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        assert.eq(result.length, 3, {result});
        for (const doc of result) {
            assert.docEq(doc.meta, expectedMeta, {doc});
        }
    });

    it("returns empty result set when source produces metadata but 0 docs", function () {
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 0, meta: expectedMeta}},
                {$project: {name: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        assert.eq(result.length, 0, {result});
    });

    it("projects only $$SEARCH_META with no other fields", function () {
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 3, meta: expectedMeta}},
                {$project: {_id: 0, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        assert.eq(result.length, 3, {result});
        for (const doc of result) {
            assert.docEq(doc.meta, expectedMeta, {doc});
            assert(!doc.hasOwnProperty("name"), "unexpected name field", {doc});
            assert(!doc.hasOwnProperty("score"), "unexpected score field", {doc});
        }
    });

    it("returns all docs across getMore batches with metadata bound on every doc", function () {
        const numDocs = 150;
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs, meta: expectedMeta}},
                {$project: {name: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();
        assert.eq(result.length, numDocs, {result});
        for (let i = 0; i < numDocs; i++) {
            assert.docEq(result[i].meta, expectedMeta, {i, result: result[i]});
        }
    });

    it("allows accessing $$SEARCH_META inside a $facet subpipeline", function () {
        const result = coll
            .aggregate([
                {$extensionMultiStream: {numDocs: 3, meta: expectedMeta}},
                {
                    $facet: {
                        meta: [{$replaceWith: "$$SEARCH_META"}, {$limit: 1}],
                        docs: [{$project: {name: 1}}],
                    },
                },
            ])
            .toArray();
        assert.eq(result.length, 1, {result});
        assert.docEq(result[0].meta, [expectedMeta], {result});
        assert.eq(result[0].docs.length, 3, {result});
        for (const doc of result[0].docs) {
            assert(doc.hasOwnProperty("name"), "missing name", {doc});
        }
    });
});
