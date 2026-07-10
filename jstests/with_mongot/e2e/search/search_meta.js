/**
 * Verify that `$searchMeta` and the '$$SEARCH_META' variable surface the metadata that mongot
 * returns for a query. With real mongot, metadata is produced via the `count` and `facet`
 * operators (the mocked version of this test injected arbitrary metadata values instead).
 * E2E version of jstests/with_mongot/search_mocked/search_meta.js
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

const collName = jsTestName();
const coll = db.getCollection(collName);

const indexName = "search_meta_index";
const genres = ["Drama", "Comedy", "Romance"];

describe("$searchMeta", function () {
    before(function () {
        coll.drop();

        let docs = [];
        for (let i = 0; i < 9; i++) {
            docs.push({_id: i, year: 2000 + i, genre: genres[i % 3]});
        }
        assert.commandWorked(coll.insertMany(docs));

        createSearchIndex(coll, {
            name: indexName,
            definition: {
                mappings: {
                    dynamic: false,
                    fields: {year: {type: "number"}, genre: {type: "stringFacet"}},
                },
            },
        });
    });

    after(function () {
        dropSearchIndex(coll, {name: indexName});
        coll.drop();
    });

    it("should return the count metadata for a query", function () {
        const results = coll
            .aggregate([
                {
                    $searchMeta: {
                        index: indexName,
                        range: {path: "year", gte: 2000, lt: 2100},
                        count: {type: "total"},
                    },
                },
            ])
            .toArray();
        assert.eq(results, [{count: {total: NumberLong(9)}}]);
    });

    it("should return metadata even when no documents match", function () {
        const results = coll
            .aggregate([
                {
                    $searchMeta: {
                        index: indexName,
                        range: {path: "year", gte: 3000, lt: 3100},
                        count: {type: "total"},
                    },
                },
            ])
            .toArray();
        assert.eq(results, [{count: {total: NumberLong(0)}}]);
    });

    it("should return facet buckets", function () {
        const results = coll
            .aggregate([
                {
                    $searchMeta: {
                        index: indexName,
                        facet: {
                            operator: {range: {path: "year", gte: 2000, lt: 2100}},
                            facets: {genresFacet: {type: "string", path: "genre"}},
                        },
                        count: {type: "total"},
                    },
                },
            ])
            .toArray();

        assert.eq(results.length, 1, results);
        assert.eq(results[0].count, {total: NumberLong(9)}, results);
        assertArrayEq({
            actual: results[0].facet.genresFacet.buckets,
            expected: [
                {_id: "Drama", count: NumberLong(3)},
                {_id: "Comedy", count: NumberLong(3)},
                {_id: "Romance", count: NumberLong(3)},
            ],
        });
    });

    it("should expose metadata to $$SEARCH_META after a $search stage", function () {
        const results = coll
            .aggregate([
                {
                    $search: {
                        index: indexName,
                        range: {path: "year", gte: 2000, lt: 2100},
                        count: {type: "total"},
                    },
                },
                {$limit: 1},
                {$project: {_id: 1, meta: "$$SEARCH_META"}},
            ])
            .toArray();

        assert.eq(results.length, 1, results);
        assert.eq(results[0].meta.count, {total: NumberLong(9)}, results);
    });
});
