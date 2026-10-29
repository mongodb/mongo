/**
 * Tests $search and $searchMeta in $lookup and $unionWith sub-pipelines using a real mongot.
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

const searchColl = db[jsTestName() + "_search"];
const baseColl = db[jsTestName() + "_base"];
const indexName = jsTestName() + "_index";

const cakeQuery = {index: indexName, text: {query: "cake", path: "title"}, count: {type: "total"}};
const cookiesQuery = {
    index: indexName,
    text: {query: "cookies", path: "title"},
    count: {type: "total"},
};

describe("$search and $searchMeta in $lookup and $unionWith", function () {
    before(function () {
        searchColl.drop();
        baseColl.drop();

        assert.commandWorked(
            searchColl.insertMany([
                {_id: 1, title: "chocolate cake", category: "dessert"},
                {_id: 2, title: "vanilla cake", category: "dessert"},
                {_id: 3, title: "red velvet cake", category: "dessert"},
                {_id: 4, title: "carrot cake", category: "dessert"},
                {_id: 5, title: "chocolate cookies", category: "snack"},
                {_id: 6, title: "oatmeal cookies", category: "snack"},
                {_id: 7, title: "sugar cookies", category: "snack"},
                {_id: 8, title: "banana bread", category: "bread"},
            ]),
        );

        assert.commandWorked(
            baseColl.insertMany([
                {_id: 100, localField: "chocolate cake", joinKey: "dessert"},
                {_id: 101, localField: "oatmeal cookies", joinKey: "snack"},
                {_id: 102, localField: "banana bread", joinKey: "bread"},
            ]),
        );

        createSearchIndex(searchColl, {
            name: indexName,
            definition: {mappings: {dynamic: true}},
        });
    });

    after(function () {
        dropSearchIndex(searchColl, {name: indexName});
        searchColl.drop();
        baseColl.drop();
    });

    it("runs $search inside uncorrelated $lookup pipeline", function () {
        const results = baseColl
            .aggregate([
                {
                    $lookup: {
                        from: searchColl.getName(),
                        pipeline: [{$search: cakeQuery}, {$project: {_id: 1, title: 1}}],
                        as: "matches",
                    },
                },
            ])
            .toArray();

        assert.eq(results.length, 3, {results});
        const expectedMatches = [
            {_id: 1, title: "chocolate cake"},
            {_id: 2, title: "vanilla cake"},
            {_id: 3, title: "red velvet cake"},
            {_id: 4, title: "carrot cake"},
        ];
        for (const doc of results) {
            assertArrayEq({actual: doc.matches, expected: expectedMatches});
        }
    });

    it("runs $search inside correlated $lookup pipeline", function () {
        const results = baseColl
            .aggregate([
                {
                    $lookup: {
                        from: searchColl.getName(),
                        let: {local: "$localField"},
                        pipeline: [
                            {$search: cakeQuery},
                            {$match: {$expr: {$eq: ["$title", "$$local"]}}},
                            {$project: {_id: 1, title: 1}},
                        ],
                        as: "matches",
                    },
                },
            ])
            .toArray();

        assert.eq(results.length, 3, {results});
        const byId = {};
        for (const doc of results) {
            byId[doc._id] = doc.matches;
        }
        assert.eq(byId[100].length, 1, {matches: byId[100]});
        assert.eq(byId[100][0].title, "chocolate cake");
        assert.eq(byId[101].length, 0, {matches: byId[101]});
        assert.eq(byId[102].length, 0, {matches: byId[102]});
    });

    it("runs $search inside $lookup with localField/foreignField", function () {
        const results = baseColl
            .aggregate([
                {
                    $lookup: {
                        from: searchColl.getName(),
                        localField: "joinKey",
                        foreignField: "category",
                        pipeline: [{$search: cakeQuery}],
                        as: "matches",
                    },
                },
            ])
            .toArray();

        assert.eq(results.length, 3, {results});
        const byId = {};
        for (const doc of results) {
            byId[doc._id] = doc.matches;
        }
        assert.eq(byId[100].length, 4, {matches: byId[100]});
        assert.eq(byId[101].length, 0, {matches: byId[101]});
        assert.eq(byId[102].length, 0, {matches: byId[102]});
    });

    it("runs $search inside $unionWith pipeline", function () {
        const results = baseColl
            .aggregate([
                {$project: {_id: 1, localField: 1}},
                {
                    $unionWith: {
                        coll: searchColl.getName(),
                        pipeline: [{$search: cakeQuery}, {$project: {_id: 1, title: 1}}],
                    },
                },
            ])
            .toArray();

        assert.eq(results.length, 7, {results});
        const resultIds = results.map((d) => d._id).sort((a, b) => a - b);
        assert.sameMembers(resultIds, [1, 2, 3, 4, 100, 101, 102]);
    });

    it("scopes $$SEARCH_META correctly across $search and $unionWith sub-pipeline", function () {
        const results = searchColl
            .aggregate([
                {$search: cookiesQuery},
                {$project: {_id: 1, title: 1, meta: "$$SEARCH_META"}},
                {
                    $unionWith: {
                        coll: searchColl.getName(),
                        pipeline: [
                            {$search: cakeQuery},
                            {$project: {_id: 1, title: 1, meta: "$$SEARCH_META"}},
                        ],
                    },
                },
            ])
            .toArray();

        assert.eq(results.length, 7, {results});
        const outer = results.filter((d) => d._id >= 5 && d._id <= 7);
        const inner = results.filter((d) => d._id >= 1 && d._id <= 4);
        assert.eq(outer.length, 3, {outer});
        assert.eq(inner.length, 4, {inner});
        for (const doc of outer) {
            assert.eq(Number(doc.meta.count.total), 3, {doc});
        }
        for (const doc of inner) {
            assert.eq(Number(doc.meta.count.total), 4, {doc});
        }
    });

    it("runs $searchMeta inside $lookup sub-pipeline", function () {
        const results = baseColl
            .aggregate([
                {$project: {_id: 1}},
                {
                    $lookup: {
                        from: searchColl.getName(),
                        pipeline: [{$searchMeta: cakeQuery}],
                        as: "meta",
                    },
                },
            ])
            .toArray();

        assert.eq(results.length, 3, {results});
        for (const doc of results) {
            assert.eq(doc.meta.length, 1, {doc});
            assert.eq(Number(doc.meta[0].count.total), 4, {doc});
        }
    });

    it("runs $searchMeta inside $unionWith sub-pipeline", function () {
        const results = baseColl
            .aggregate([
                {$project: {_id: 1}},
                {
                    $unionWith: {
                        coll: searchColl.getName(),
                        pipeline: [{$searchMeta: cakeQuery}],
                    },
                },
            ])
            .toArray();

        assert.eq(results.length, 4, {results});
        const baseDocs = results.filter((d) => d.hasOwnProperty("_id") && d._id >= 100);
        const metaDocs = results.filter((d) => d.hasOwnProperty("count"));
        assert.eq(baseDocs.length, 3, {baseDocs});
        assert.eq(metaDocs.length, 1, {metaDocs});
        assert.eq(Number(metaDocs[0].count.total), 4, {metaDocs});
    });

    it("combines top-level $search with $unionWith containing $searchMeta", function () {
        const results = searchColl
            .aggregate([
                {$search: cookiesQuery},
                {$project: {_id: 1, meta: "$$SEARCH_META"}},
                {
                    $unionWith: {
                        coll: searchColl.getName(),
                        pipeline: [{$searchMeta: cakeQuery}],
                    },
                },
            ])
            .toArray();

        const searchDocs = results.filter((d) => d.hasOwnProperty("_id"));
        const metaDocs = results.filter((d) => d.hasOwnProperty("count"));
        assert.eq(searchDocs.length, 3, {searchDocs});
        for (const doc of searchDocs) {
            assert.eq(Number(doc.meta.count.total), 3, {doc});
        }
        assert.eq(metaDocs.length, 1, {metaDocs});
        assert.eq(Number(metaDocs[0].count.total), 4, {metaDocs});
    });

    it("combines $searchMeta with $unionWith containing $searchMeta", function () {
        const results = searchColl
            .aggregate([
                {$searchMeta: cookiesQuery},
                {
                    $unionWith: {
                        coll: searchColl.getName(),
                        pipeline: [{$searchMeta: cakeQuery}],
                    },
                },
            ])
            .toArray();

        assert.eq(results.length, 2, {results});
        const totals = results.map((d) => Number(d.count.total)).sort((a, b) => a - b);
        assert.eq(totals, [3, 4], {results});
    });

    it("runs $search inside nested $lookup", function () {
        const results = baseColl
            .aggregate([
                {$project: {_id: 1}},
                {
                    $lookup: {
                        from: baseColl.getName(),
                        pipeline: [
                            {$project: {_id: 1}},
                            {
                                $lookup: {
                                    from: searchColl.getName(),
                                    pipeline: [
                                        {$search: cakeQuery},
                                        {$project: {_id: 1, title: 1}},
                                    ],
                                    as: "searchResults",
                                },
                            },
                        ],
                        as: "outer",
                    },
                },
            ])
            .toArray();

        assert.eq(results.length, 3, {results});
        for (const doc of results) {
            assert.eq(doc.outer.length, 3, {doc});
            for (const innerDoc of doc.outer) {
                assert.eq(innerDoc.searchResults.length, 4, {innerDoc});
                const innerIds = innerDoc.searchResults.map((d) => d._id).sort((a, b) => a - b);
                assert.eq(innerIds, [1, 2, 3, 4], {innerDoc});
            }
        }
    });
});
