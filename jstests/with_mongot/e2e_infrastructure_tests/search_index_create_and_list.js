/**
 * Tests search index CRUD operations against a real mongot instance.
 *
 * Covers create (type variations), listSearchIndexes command, and $listSearchIndexes aggregation
 * stage.
 */
import {describe, it, before, after, afterEach} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.searchIndexCrudColl;

describe("search index CRUD operations", function () {
    before(function () {
        coll.drop();
        assert.commandWorked(
            coll.insertMany([
                {text: "alpha", category: "a", embedding: [1.0, 0.0, 0.0]},
                {text: "bravo", category: "b", embedding: [0.0, 1.0, 0.0]},
                {text: "charlie", category: "c", embedding: [0.0, 0.0, 1.0]},
            ]),
        );
    });

    afterEach(function () {
        const indexes = coll.aggregate([{$listSearchIndexes: {}}]).toArray();
        for (const idx of indexes) {
            dropSearchIndex(coll, {name: idx.name});
        }
    });

    after(function () {
        coll.drop();
    });

    describe("createSearchIndexes", function () {
        it("creates an index with type 'search'", function () {
            createSearchIndex(coll, {
                name: "searchTypeIdx",
                type: "search",
                definition: {mappings: {dynamic: true}},
            });
            const result = coll
                .aggregate([{$listSearchIndexes: {name: "searchTypeIdx"}}])
                .toArray();
            assert.eq(result.length, 1, {msg: "expected exactly one index", result});
            assert.eq(result[0].name, "searchTypeIdx");
            dropSearchIndex(coll, {name: "searchTypeIdx"});
        });

        it("creates an index with type 'vectorSearch'", function () {
            createSearchIndex(coll, {
                name: "vectorIdx",
                type: "vectorSearch",
                definition: {
                    fields: [
                        {
                            type: "vector",
                            path: "embedding",
                            numDimensions: 3,
                            similarity: "cosine",
                        },
                    ],
                },
            });
            const result = coll.aggregate([{$listSearchIndexes: {name: "vectorIdx"}}]).toArray();
            assert.eq(result.length, 1, {msg: "expected exactly one index", result});
            assert.eq(result[0].name, "vectorIdx");
            dropSearchIndex(coll, {name: "vectorIdx"});
        });

        it("creates an index without explicit type", function () {
            createSearchIndex(coll, {name: "noTypeIdx", definition: {mappings: {dynamic: true}}});
            const result = coll.aggregate([{$listSearchIndexes: {name: "noTypeIdx"}}]).toArray();
            assert.eq(result.length, 1, {msg: "expected exactly one index", result});
            assert.eq(result[0].name, "noTypeIdx");
            dropSearchIndex(coll, {name: "noTypeIdx"});
        });
    });

    describe("listSearchIndexes command", function () {
        it("lists all indexes", function () {
            createSearchIndex(coll, {name: "listAllIdx", definition: {mappings: {dynamic: true}}});
            const result = assert.commandWorked(
                testDb.runCommand({listSearchIndexes: coll.getName()}),
            );
            assert.gte(result.cursor.firstBatch.length, 1, {
                msg: "expected at least one index",
                result,
            });
            const found = result.cursor.firstBatch.some((idx) => idx.name === "listAllIdx");
            assert(found, "expected listAllIdx in results", {firstBatch: result.cursor.firstBatch});
            dropSearchIndex(coll, {name: "listAllIdx"});
        });

        it("lists by name filter", function () {
            createSearchIndex(coll, {
                name: "listByNameIdx",
                definition: {mappings: {dynamic: true}},
            });
            const result = assert.commandWorked(
                testDb.runCommand({listSearchIndexes: coll.getName(), name: "listByNameIdx"}),
            );
            assert.eq(result.cursor.firstBatch.length, 1, {
                msg: "expected exactly one index for name filter",
                result,
            });
            assert.eq(result.cursor.firstBatch[0].name, "listByNameIdx");
            dropSearchIndex(coll, {name: "listByNameIdx"});
        });
    });

    describe("$listSearchIndexes aggregation stage", function () {
        it("lists all indexes via agg stage", function () {
            createSearchIndex(coll, {name: "aggListIdx", definition: {mappings: {dynamic: true}}});
            const result = coll.aggregate([{$listSearchIndexes: {}}]).toArray();
            assert.gte(result.length, 1, {msg: "expected at least one index", result});
            const found = result.some((idx) => idx.name === "aggListIdx");
            assert(found, "expected aggListIdx in results", {result});
            dropSearchIndex(coll, {name: "aggListIdx"});
        });

        it("lists by name filter via agg stage", function () {
            createSearchIndex(coll, {
                name: "aggFilterIdx",
                definition: {mappings: {dynamic: true}},
            });
            const result = coll.aggregate([{$listSearchIndexes: {name: "aggFilterIdx"}}]).toArray();
            assert.eq(result.length, 1, {msg: "expected exactly one index", result});
            assert.eq(result[0].name, "aggFilterIdx");
            dropSearchIndex(coll, {name: "aggFilterIdx"});
        });

        it("returns empty result when no search indexes exist", function () {
            const emptyTestColl = testDb.emptySearchIndexColl;
            emptyTestColl.drop();
            assert.commandWorked(emptyTestColl.insertMany([{x: 1}, {x: 2}]));
            const result = emptyTestColl.aggregate([{$listSearchIndexes: {}}]).toArray();
            assert.eq(result.length, 0, {msg: "expected empty result", result});
            emptyTestColl.drop();
        });
    });
});
