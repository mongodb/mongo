/**
 * Tests the basic operation of a `$search` aggregation stage with real mongot.
 * E2E version of jstests/with_mongot/search_mocked/search.js
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

const collName = jsTestName();
const coll = db.getCollection(collName);

const indexName = "search_index";
const searchStage = {$search: {index: indexName, text: {query: "cakes", path: "title"}}};

describe("$search", function () {
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

    it("should return all matching documents", function () {
        const results = coll.aggregate([searchStage]).toArray();
        assertArrayEq({
            actual: results,
            expected: [
                {_id: 1, title: "cakes"},
                {_id: 2, title: "cookies and cakes"},
                {_id: 5, title: "cakes and oranges"},
                {_id: 6, title: "cakes and apples"},
                {_id: 8, title: "cakes and kale"},
            ],
        });
    });

    it("should return all results across multiple batches, sorted by score", function () {
        const res = assert.commandWorked(
            db.runCommand({
                aggregate: collName,
                pipeline: [searchStage, {$project: {_id: 1, score: {$meta: "searchScore"}}}],
                cursor: {batchSize: 2},
            }),
        );

        let results = res.cursor.firstBatch;
        let cursorId = res.cursor.id;
        while (cursorId != 0) {
            const getMoreRes = assert.commandWorked(
                db.runCommand({getMore: cursorId, collection: collName}),
            );
            results = results.concat(getMoreRes.cursor.nextBatch);
            cursorId = getMoreRes.cursor.id;
        }

        assert.eq(results.length, 5, results);
        for (let i = 1; i < results.length; i++) {
            assert.gte(results[i - 1].score, results[i].score, results);
        }
    });

    it("should propagate errors from mongot", function () {
        // 'notAnOperator' is not a valid mongot operator, so mongot returns an error.
        assert.commandFailed(
            db.runCommand({
                aggregate: collName,
                pipeline: [{$search: {index: indexName, notAnOperator: {path: "title"}}}],
                cursor: {},
            }),
        );
    });

    it("should return no results on an empty collection", function () {
        const emptyColl = db.getCollection(collName + "_empty");
        emptyColl.drop();

        // Create the collection by inserting and immediately deleting a document so that it
        // exists with a UUID.
        assert.commandWorked(emptyColl.insertOne({_id: "temp"}));
        assert.commandWorked(emptyColl.deleteOne({_id: "temp"}));

        const res = assert.commandWorked(
            db.runCommand({
                aggregate: emptyColl.getName(),
                pipeline: [searchStage],
                cursor: {},
            }),
        );
        assert.eq(res.cursor.firstBatch, []);

        emptyColl.drop();
    });

    it("should return no results on a nonexistent collection", function () {
        const results = db.getCollection(collName + "_does_not_exist").aggregate([searchStage]);
        assert.eq(results.toArray(), []);
    });

    it("should fail on non-local read concern", function () {
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: collName,
                pipeline: [searchStage],
                cursor: {},
                readConcern: {level: "majority"},
            }),
            [ErrorCodes.InvalidOptions, ErrorCodes.ReadConcernMajorityNotEnabled],
        );
    });
});
