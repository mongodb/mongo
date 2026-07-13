/**
 * Tests that "searchScore", "searchHighlights", and "searchScoreDetails" metadata is properly
 * plumbed through the $search agg stage, including across multiple getMore batches.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

const collName = jsTestName();
const coll = db.getCollection(collName);

const indexName = jsTestName() + "_index";
const pipeline = [
    {
        $search: {
            index: indexName,
            text: {query: "cakes", path: "title"},
            highlight: {path: "title"},
            scoreDetails: true,
        },
    },
    {
        $project: {
            _id: 1,
            score: {$meta: "searchScore"},
            highlights: {$meta: "searchHighlights"},
            scoreInfo: {$meta: "searchScoreDetails"},
        },
    },
];

describe("$search metadata", function () {
    before(function () {
        coll.drop();

        assert.commandWorked(
            coll.insertMany([
                {_id: 0, title: "cakes"},
                {_id: 1, title: "cookies and cakes"},
                {_id: 2, title: "vegetables"},
                {_id: 3, title: "oranges"},
                {_id: 4, title: "cakes and oranges"},
                {_id: 5, title: "cakes and apples"},
                {_id: 6, title: "cakes and kale"},
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

    function assertMetadataPopulated(doc) {
        assert(doc.hasOwnProperty("score"), "missing searchScore", {doc});
        assert.gt(doc.score, 0, doc);

        assert(doc.hasOwnProperty("highlights"), "missing searchHighlights", {doc});
        assert.gt(doc.highlights.length, 0, doc);
        for (const highlight of doc.highlights) {
            assert.eq(highlight.path, "title", doc);
            assert(
                highlight.texts.some((text) => text.type === "hit" && text.value.includes("cakes")),
                "expected a highlight hit on the search term",
                {doc},
            );
        }

        assert(doc.hasOwnProperty("scoreInfo"), "missing searchScoreDetails", {doc});
        assert.eq(doc.scoreInfo.value, doc.score, doc);
        assert(doc.scoreInfo.hasOwnProperty("description"), "missing score description", {doc});
        assert(doc.scoreInfo.hasOwnProperty("details"), "missing score details", {doc});
    }

    it("should populate searchScore, searchHighlights, and searchScoreDetails", function () {
        const results = coll.aggregate(pipeline).toArray();
        assert.eq(results.length, 5, results);
        results.forEach(assertMetadataPopulated);
    });

    it("should pass metadata along correctly when there are multiple batches", function () {
        const res = assert.commandWorked(
            db.runCommand({aggregate: collName, pipeline, cursor: {batchSize: 2}}),
        );

        let results = res.cursor.firstBatch;
        let cursorId = res.cursor.id;
        while (cursorId != 0) {
            const getMoreRes = assert.commandWorked(
                db.runCommand({getMore: cursorId, collection: collName, batchSize: 2}),
            );
            results = results.concat(getMoreRes.cursor.nextBatch);
            cursorId = getMoreRes.cursor.id;
        }

        assert.eq(results.length, 5, results);
        results.forEach(assertMetadataPopulated);
    });
});
