/**
 * E2E tests for $search with returnStoredSource: stored field filtering,
 * score metadata, and $$SEARCH_META via count.
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
const indexName = jsTestName() + "_index";

const searchQuery = {
    index: indexName,
    text: {query: "cakes", path: "title"},
    returnStoredSource: true,
    count: {type: "total"},
};

const docs = [
    {_id: 1, title: "cakes and cupcakes", category: "baking", tasty: true, internalNote: "promo"},
    {_id: 2, title: "chocolate cakes", category: "baking", tasty: true},
    {_id: 3, title: "savory cakes", category: "cooking", tasty: false},
    {_id: 4, title: "bread loaves", category: "baking", tasty: true},
    {_id: 5, title: "lemon cakes", category: "dessert"},
];

const expectedMatchingIds = [1, 2, 3, 5];

describe("$search returnStoredSource", function () {
    before(function () {
        coll.drop();
        assert.commandWorked(coll.insertMany(docs));

        createSearchIndex(coll, {
            name: indexName,
            definition: {
                mappings: {dynamic: false, fields: {title: {type: "string"}}},
                storedSource: {include: ["title", "category", "tasty"]},
            },
        });
    });

    after(function () {
        dropSearchIndex(coll, {name: indexName});
        coll.drop();
    });

    it("returns only stored fields and excludes non-stored fields", function () {
        const results = coll
            .aggregate([
                {$search: searchQuery},
                {$project: {_id: 1, title: 1, category: 1, tasty: 1, internalNote: 1}},
            ])
            .toArray();

        assert.eq(results.length, expectedMatchingIds.length, results);
        assertArrayEq({actual: results.map((doc) => doc._id), expected: expectedMatchingIds});

        for (const doc of results) {
            assert(doc.hasOwnProperty("title"), "missing title", {doc});
            assert(doc.hasOwnProperty("category"), "missing category", {doc});
            assert(
                !doc.hasOwnProperty("internalNote"),
                "internalNote should not be in stored source",
                {doc},
            );
            if (doc._id !== 5) {
                assert(doc.hasOwnProperty("tasty"), "missing tasty", {doc});
            }
        }
    });

    it("preserves search score metadata and returns results in score order", function () {
        const results = coll
            .aggregate([
                {$search: searchQuery},
                {$project: {_id: 1, title: 1, score: {$meta: "searchScore"}}},
            ])
            .toArray();

        assert.eq(results.length, expectedMatchingIds.length, results);

        for (let i = 0; i < results.length; i++) {
            assert.gt(results[i].score, 0, results);
            if (i > 0) {
                assert.gte(results[i - 1].score, results[i].score, results);
            }
        }
    });

    it("exposes real $$SEARCH_META with count", function () {
        const results = coll
            .aggregate([{$search: searchQuery}, {$project: {_id: 1, meta: "$$SEARCH_META"}}])
            .toArray();

        assert.eq(results.length, expectedMatchingIds.length, results);
        const expectedMeta = {count: {total: NumberLong(expectedMatchingIds.length)}};
        for (const doc of results) {
            assert.docEq(expectedMeta, doc.meta, results);
        }
    });

    it("handles document missing an optional stored field", function () {
        const results = coll
            .aggregate([
                {$search: searchQuery},
                {$project: {_id: 1, title: 1, category: 1, tasty: 1}},
            ])
            .toArray();

        const doc5 = results.find((doc) => doc._id === 5);
        assert(doc5, "doc with _id 5 should be in results", {results});
        assert(doc5.hasOwnProperty("title"), "missing title on doc 5", {doc5});
        assert(doc5.hasOwnProperty("category"), "missing category on doc 5", {doc5});
        assert(!doc5.hasOwnProperty("tasty"), "tasty should be absent on doc 5", {doc5});
    });
});
