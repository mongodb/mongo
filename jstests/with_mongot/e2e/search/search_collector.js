/**
 * Verify that `$search` sets the '$$SEARCH_META' variable, produced via the `count` operator:
 * every result document observes the same merged metadata in score order, unset metadata subfields
 * evaluate to missing, and the variable survives a split pipeline.
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

const collName = jsTestName();
const coll = db.getCollection(collName);

const indexName = jsTestName() + "_index";
// Matches the 5 documents whose title contains "cakes"; the count operator makes mongot return
// SEARCH_META.
const searchQuery = {
    index: indexName,
    text: {query: "cakes", path: "title"},
    count: {type: "total"},
};
const expectedMeta = {count: {total: NumberLong(5)}};
const expectedIds = [1, 2, 5, 6, 8];

describe("$search sets $$SEARCH_META", function () {
    before(function () {
        coll.drop();

        assert.commandWorked(
            coll.insertMany([
                {_id: 1, title: "cakes"},
                {_id: 2, title: "cookies and cakes"},
                {_id: 3, title: "vegetables"},
                {_id: 4, title: "oranges"},
                {_id: 5, title: "cakes and oranges"},
                {_id: 6, title: "cakes and apples"},
                {_id: 7, title: "apples"},
                {_id: 8, title: "cakes and kale"},
            ]),
        );

        createSearchIndex(coll, {
            name: indexName,
            definition: {mappings: {dynamic: false, fields: {title: {type: "string"}}}},
        });
    });

    after(function () {
        dropSearchIndex(coll, {name: indexName});
        coll.drop();
    });

    it("should expose the same metadata to every result document, in score order", function () {
        const results = coll
            .aggregate([
                {$search: searchQuery},
                {$project: {_id: 1, score: {$meta: "searchScore"}, meta: "$$SEARCH_META"}},
            ])
            .toArray();

        assert.eq(results.length, expectedIds.length, results);
        assertArrayEq({actual: results.map((doc) => doc._id), expected: expectedIds});
        for (let i = 0; i < results.length; i++) {
            assert.docEq(expectedMeta, results[i].meta, results);
            if (i > 0) {
                // Results are merged in mongot score order.
                assert.gte(results[i - 1].score, results[i].score, results);
            }
        }
    });

    it("should evaluate unset metadata fields to missing", function () {
        const results = coll
            .aggregate([
                {$search: searchQuery},
                {$project: {_id: 1, missingMeta: "$$SEARCH_META.missing"}},
            ])
            .toArray();

        assertArrayEq({actual: results, expected: expectedIds.map((_id) => ({_id}))});
    });

    it("should work with a stage that forces a merge", function () {
        // $group needs visibility across all shards' documents to produce correct final groups, so
        // mongos splits it into a per-shard partial-group stage plus a merging stage that combines
        // the partial results. This confirms the $$SEARCH_META-derived value already resolved into
        // each document earlier in the pipeline survives intact across that shard/merge split.
        const results = coll
            .aggregate([
                {$search: searchQuery},
                {$project: {_id: 1, meta: "$$SEARCH_META"}},
                {$group: {_id: "$meta", ids: {$addToSet: "$_id"}}},
            ])
            .toArray();

        assert.eq(results.length, 1, results);
        assert.eq(results[0]._id, expectedMeta, results);
        assertArrayEq({actual: results[0].ids, expected: expectedIds});
    });
});
