/**
 * E2E tests for searchSequenceToken: $project, $addFields, $facet with
 * $$SEARCH_META, $group by token, pagination via searchAfter.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

const collName = jsTestName();
const coll = db.getCollection(collName);

const indexName = jsTestName() + "_index";

const docs = [
    {_id: 1, title: "cake recipe", sequence: 1},
    {_id: 2, title: "cake decorating", sequence: 2},
    {_id: 3, title: "cake baking tips", sequence: 3},
    {_id: 4, title: "cake frosting guide", sequence: 4},
    {_id: 5, title: "cake flavors", sequence: 5},
    {_id: 6, title: "cake layers", sequence: 6},
    {_id: 7, title: "cake tools", sequence: 7},
    {_id: 8, title: "cake ingredients", sequence: 8},
];

const searchQuery = {
    index: indexName,
    text: {query: "cake", path: "title"},
    sort: {sequence: 1},
};

function assertValidSequenceTokens(results, tokenField = "token") {
    assert.gt(results.length, 0, results);

    const tokens = results.map((result) => {
        assert(result.hasOwnProperty(tokenField), "missing token field", {result, tokenField});
        assert.eq(typeof result[tokenField], "string", result);
        assert.gt(result[tokenField].length, 0, result);
        return result[tokenField];
    });

    assert.eq(new Set(tokens).size, tokens.length, results);
}

describe("search sequence token", function () {
    before(function () {
        coll.drop();
        assert.commandWorked(coll.insertMany(docs));
        createSearchIndex(coll, {
            name: indexName,
            definition: {
                mappings: {
                    dynamic: false,
                    fields: {
                        title: {type: "string"},
                        sequence: {type: "number"},
                    },
                },
            },
        });
    });

    after(function () {
        dropSearchIndex(coll, {name: indexName});
        coll.drop();
    });

    it("projects searchSequenceToken via $project", function () {
        const results = coll
            .aggregate([
                {$search: searchQuery},
                {$project: {_id: 1, sequence: 1, token: {$meta: "searchSequenceToken"}}},
            ])
            .toArray();

        assert.eq(results.length, docs.length, results);
        for (let i = 0; i < results.length; i++) {
            assert.eq(results[i].sequence, i + 1, results);
        }
        assertValidSequenceTokens(results);
    });

    it("projects searchSequenceToken via $addFields", function () {
        const results = coll
            .aggregate([
                {$search: searchQuery},
                {$addFields: {token: {$meta: "searchSequenceToken"}}},
            ])
            .toArray();

        assert.eq(results.length, docs.length, results);
        results.forEach((doc) => {
            assert(doc.hasOwnProperty("title"), "missing title field", {doc});
        });
        assertValidSequenceTokens(results);
    });

    it("exposes searchSequenceToken and $$SEARCH_META inside $facet", function () {
        const results = coll
            .aggregate([
                {
                    $search: {
                        index: indexName,
                        text: {query: "cake", path: "title"},
                        count: {type: "total"},
                    },
                },
                {
                    $facet: {
                        meta: [{$replaceWith: "$$SEARCH_META"}, {$limit: 1}],
                        docs: [
                            {$limit: 3},
                            {
                                $project: {
                                    paginationToken: {$meta: "searchSequenceToken"},
                                    score: {$meta: "searchScore"},
                                },
                            },
                        ],
                    },
                },
            ])
            .toArray();

        assert.eq(results.length, 1, results);
        const facetResult = results[0];

        assert.eq(facetResult.docs.length, 3, facetResult);
        assertValidSequenceTokens(facetResult.docs, "paginationToken");
        facetResult.docs.forEach((doc) => {
            assert(doc.hasOwnProperty("score"), "missing score field", {doc});
            assert.eq(typeof doc.score, "number", doc);
        });

        assert.eq(facetResult.meta.length, 1, facetResult);
        assert(facetResult.meta[0].hasOwnProperty("count"), "missing count in meta", {
            meta: facetResult.meta[0],
        });
        assert.eq(
            Number(facetResult.meta[0].count.total),
            Number(docs.length),
            facetResult.meta[0],
        );
    });

    it("allows $group by searchSequenceToken", function () {
        const results = coll
            .aggregate([{$search: searchQuery}, {$group: {_id: {$meta: "searchSequenceToken"}}}])
            .toArray();

        assert.eq(results.length, docs.length, results);
        results.forEach((doc) => {
            assert.eq(typeof doc._id, "string", doc);
            assert.gt(doc._id.length, 0, doc);
        });
        const ids = results.map((doc) => doc._id);
        assert.eq(new Set(ids).size, ids.length, results);
    });

    it("supports pagination via searchAfter", function () {
        const pageSize = 3;

        const page1 = coll
            .aggregate([
                {$search: searchQuery},
                {$limit: pageSize},
                {$project: {_id: 1, sequence: 1, token: {$meta: "searchSequenceToken"}}},
            ])
            .toArray();

        assert.eq(page1.length, pageSize, page1);
        assertValidSequenceTokens(page1);

        const lastToken = page1[page1.length - 1].token;

        const page2 = coll
            .aggregate([
                {
                    $search: {
                        index: indexName,
                        text: {query: "cake", path: "title"},
                        sort: {sequence: 1},
                        searchAfter: lastToken,
                    },
                },
                {$limit: pageSize},
                {$project: {_id: 1, sequence: 1, token: {$meta: "searchSequenceToken"}}},
            ])
            .toArray();

        assert.eq(page2.length, pageSize, page2);
        assertValidSequenceTokens(page2);

        const page1Ids = new Set(page1.map((d) => d._id));
        page2.forEach((doc) => {
            assert(!page1Ids.has(doc._id), "page2 doc overlaps with page1", {doc, page1});
        });

        assert.gt(page2[0].sequence, page1[page1.length - 1].sequence, {page1, page2});
    });
});
