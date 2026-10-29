/**
 * Enables a failpoint that recreates an interruption on the OpCtx while planShardedSearch is
 * executing, and asserts that the correct error is thrown instead of the server segfaulting.
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_unsharded_collection,
 * ]
 */

import {getShardNames} from "jstests/libs/cluster_helpers/sharded_cluster_fixture_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

// The failpoint must be enabled on the same router that executes the aggregate.
TestData.pinToSingleMongos = true;

const testDb = db.getSiblingDB(jsTestName());
const collName = jsTestName();
const testColl = testDb.getCollection(collName);
const indexName = "search_index";
const searchQuery = {index: indexName, exists: {path: "x"}};

describe("sharded $search network interruption", function () {
    before(function () {
        const shardNames = getShardNames(testDb.getMongo());
        assert.gte(shardNames.length, 2, "Test requires at least 2 shards");
        assert.commandWorked(
            testDb.adminCommand({enableSharding: testDb.getName(), primaryShard: shardNames[0]}),
        );

        testColl.drop();
        assert.commandWorked(
            testColl.insertMany([
                {_id: 1, x: "a"},
                {_id: 20, x: "b"},
            ]),
        );

        // The collection must be sharded so that the aggregate goes through planShardedSearch.
        assert.commandWorked(
            testDb.adminCommand({shardCollection: testColl.getFullName(), key: {_id: 1}}),
        );
        assert.commandWorked(
            testDb.adminCommand({split: testColl.getFullName(), middle: {_id: 10}}),
        );
        // Every shard must own part of the collection before the search index is created:
        // createSearchIndex multicasts to all shards, and a shard with no local collection fails
        // the command.
        assert.commandWorked(
            testDb.adminCommand({
                moveChunk: testColl.getFullName(),
                find: {_id: 20},
                to: shardNames[1],
            }),
        );

        createSearchIndex(testColl, {name: indexName, definition: {mappings: {dynamic: true}}});

        assert.commandWorked(
            testDb.adminCommand({
                configureFailPoint: "shardedSearchOpCtxDisconnect",
                mode: "alwaysOn",
            }),
        );
    });

    after(function () {
        assert.commandWorked(
            testDb.adminCommand({configureFailPoint: "shardedSearchOpCtxDisconnect", mode: "off"}),
        );
        dropSearchIndex(testColl, {name: indexName});
        testColl.drop();
    });

    // The query must be one a real mongot accepts. If mongot's planShardedSearch reply beats the
    // failpoint's markKilled() -- which happens on sanitizer variants -- an invalid query surfaces
    // mongot's rejection instead of the interruption we are asserting on.
    it("should return an interruption error rather than crash", function () {
        const error = assert.throws(() => testColl.aggregate([{$search: searchQuery}]));
        assert.commandFailedWithCode(error, ErrorCodes.Interrupted);

        // Make sure the router is still up.
        assert.commandWorked(testDb.runCommand("ping"));
    });
});
