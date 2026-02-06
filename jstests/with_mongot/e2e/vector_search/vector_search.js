/**
 * Tests for the `$vectorSearch` aggregation pipeline stage with real mongot.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

const collName = jsTestName();
const coll = db.getCollection(collName);

const queryVector = [1.0, 2.0, 3.0];
const path = "x";
const numCandidates = 10;
const limit = 5;
const index = "vector_search_index";

describe("$vectorSearch", function () {
    before(function () {
        coll.drop();

        assert.commandWorked(
            coll.insertMany([
                {_id: 0, x: [1.0, 2.0, 3.0]},
                {_id: 1, x: [1.1, 2.1, 3.1]},
                {_id: 2, x: [0.9, 1.9, 2.9], color: "A"},
                {_id: 3, x: [0.8, 1.8, 2.8], color: "D"},
                {_id: 4, x: [0.7, 1.7, 2.7], extraField: true},
            ]),
        );

        createSearchIndex(coll, {
            name: index,
            type: "vectorSearch",
            definition: {
                fields: [
                    {
                        type: "vector",
                        path: path,
                        numDimensions: 3,
                        similarity: "cosine",
                    },
                    {
                        type: "filter",
                        path: "color",
                    },
                    {
                        type: "filter",
                        path: "extraField",
                    },
                ],
            },
        });
    });

    after(function () {
        dropSearchIndex(coll, {name: index});
        coll.drop();
    });

    it("should do nothing on an empty collection", function () {
        const emptyCollName = collName + "_empty";
        const emptyColl = db.getCollection(emptyCollName);

        // Create the collection by inserting and immediately deleting a document.
        // This ensures the collection exists with a UUID.
        assert.commandWorked(emptyColl.insertOne({_id: "temp"}));
        assert.commandWorked(emptyColl.deleteOne({_id: "temp"}));

        const pipeline = [{$vectorSearch: {queryVector, path, numCandidates, limit, index}}];
        const res = assert.commandWorked(db.runCommand({aggregate: emptyCollName, pipeline, cursor: {}}));
        assert.eq(res.cursor.firstBatch, []);

        emptyColl.drop();
    });

    it("should query mongot and return results", function () {
        const pipeline = [{$vectorSearch: {queryVector, path, numCandidates, limit, index}}];

        const res = assert.commandWorked(db.runCommand({aggregate: collName, pipeline, cursor: {}}));
        const results = res.cursor.firstBatch;
        assert.eq(results.length, 5);

        results.forEach((doc) => {
            assert(doc.hasOwnProperty("_id"));
            assert(doc.hasOwnProperty("x"));
        });
    });

    it("should respect the limit parameter", function () {
        const pipeline = [{$vectorSearch: {queryVector, path, numCandidates, limit: 1, index}}];

        const res = assert.commandWorked(db.runCommand({aggregate: collName, pipeline, cursor: {}}));
        assert.eq(res.cursor.firstBatch.length, 1);
    });

    it("should work with filter", function () {
        const filter = {"$or": [{"color": {"$gt": "C"}}, {"color": {"$lt": "C"}}]};
        const pipeline = [{$vectorSearch: {queryVector, path, numCandidates, limit: 10, index, filter}}];

        const res = assert.commandWorked(db.runCommand({aggregate: collName, pipeline, cursor: {}}));
        const results = res.cursor.firstBatch;

        results.forEach((doc) => {
            if (doc.hasOwnProperty("color")) {
                assert(doc.color > "C" || doc.color < "C");
            }
        });
    });

    it("should populate vectorSearchScore metadata", function () {
        const pipeline = [
            {$vectorSearch: {queryVector, path, numCandidates, limit, index}},
            {$project: {_id: 1, score: {$meta: "vectorSearchScore"}}},
        ];

        const res = assert.commandWorked(db.runCommand({aggregate: collName, pipeline, cursor: {}}));
        const results = res.cursor.firstBatch;
        assert.eq(results.length, 5);

        results.forEach((doc) => {
            assert(doc.hasOwnProperty("score"));
            assert.gt(doc.score, 0);
        });
    });

    it("should propagate errors from mongot", function () {
        const pipeline = [{$vectorSearch: {queryVector, path, numCandidates: -1, limit, index}}];

        assert.commandFailed(db.runCommand({aggregate: collName, pipeline, cursor: {}}));
    });

    it("should handle multiple batches correctly", function () {
        const pipeline = [
            {$vectorSearch: {queryVector, path, numCandidates, limit: 5, index}},
            {$project: {_id: 1, score: {$meta: "vectorSearchScore"}}},
        ];

        const res = assert.commandWorked(db.runCommand({aggregate: collName, pipeline, cursor: {batchSize: 2}}));

        let results = res.cursor.firstBatch;
        let cursorId = res.cursor.id;

        while (cursorId != 0) {
            const getMoreRes = assert.commandWorked(db.runCommand({getMore: cursorId, collection: collName}));
            results = results.concat(getMoreRes.cursor.nextBatch);
            cursorId = getMoreRes.cursor.id;
        }

        assert.lte(results.length, 5);

        for (let i = 1; i < results.length; i++) {
            assert.gte(results[i - 1].score, results[i].score);
        }
    });

    it("should fail on non-local read concern", function () {
        const pipeline = [{$vectorSearch: {queryVector, path, numCandidates, limit: 5, index}}];

        assert.commandFailedWithCode(
            db.runCommand({aggregate: collName, pipeline, cursor: {}, readConcern: {level: "majority"}}),
            [ErrorCodes.InvalidOptions, ErrorCodes.ReadConcernMajorityNotEnabled],
        );
    });
});
