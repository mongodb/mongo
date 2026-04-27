/**
 * Verifies that reshardCollection fails when run on a collection that has a MongoDB Search index.
 *
 * @tags: [
 *   requires_sharding,
 *   resource_intensive,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";
import {getShardNames} from "jstests/libs/cluster_helpers/sharded_cluster_fixture_helpers.js";

const testDb = db.getSiblingDB(jsTestName());
const collName = jsTestName() + "_reshard_with_search_index";
const coll = testDb.getCollection(collName);
const searchIndexName = "test_search_index";

const shardNames = getShardNames(testDb.getMongo());
assert.gte(shardNames.length, 2, "Test requires at least 2 shards");

const setUp = function (shardColl = true) {
    return function () {
        coll.drop();

        // Enable sharding for the test DB (let the fixture choose the primary shard).
        assert.commandWorked(testDb.adminCommand({enableSharding: testDb.getName(), primaryShard: shardNames[0]}));

        // Seed some data before sharding.
        assert.commandWorked(
            coll.insert([
                {_id: 1, a: 1, x: "ow"},
                {_id: 2, a: 2, x: "now"},
                {_id: 3, a: 3, x: "brown"},
                {_id: 4, a: 4, x: "cow"},
                {_id: 11, a: 5, x: "brown"},
                {_id: 12, a: 6, x: "cow"},
                {_id: 13, a: 7, x: "brown"},
                {_id: 14, a: 8, x: "cow"},
            ]),
        );

        if (shardColl) {
            // Shard the collection on {_id: 1} to make it eligible for resharding.
            assert.commandWorked(testDb.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
            assert.commandWorked(testDb.adminCommand({split: coll.getFullName(), middle: {_id: 10}}));
            assert.commandWorked(
                testDb.adminCommand({
                    moveChunk: coll.getFullName(),
                    find: {_id: 11},
                    to: shardNames[1],
                    _waitForDelete: true,
                }),
            );
        }

        // Create a MongoDB Search index on the sharded collection.
        // Use dynamic mapping so the index covers all documents.
        createSearchIndex(coll, {
            name: searchIndexName,
            definition: {
                mappings: {
                    dynamic: true,
                },
            },
        });
    };
};

const testWithExpectedFailure = function (cmd) {
    return function () {
        const res = testDb.adminCommand(cmd);
        assert.commandFailedWithCode(res, ErrorCodes.IllegalOperation);
    };
};

const testAfterDroppingIndex = function (cmd) {
    return function () {
        // Drop the search index so the collection no longer has any MongoDB Search indexes.
        // also verifies that search index was not deleted by resharding in previous test
        dropSearchIndex(coll, {name: searchIndexName});
        assert.commandWorked(testDb.adminCommand(cmd));
    };
};

const tearDown = function () {
    return function () {
        // Best-effort cleanup; ignore failures if the index was already dropped.
        try {
            dropSearchIndex(coll, {name: searchIndexName});
        } catch (e) {
            // Ignore.
        }
        coll.drop();
    };
};

const cmdsToTest = [
    {
        name: "reshardCollection",
        cmd: {
            reshardCollection: coll.getFullName(),
            key: {a: 1},
            numInitialChunks: 2,
        },
    },
    {
        name: "unshardCollection",
        cmd: {
            unshardCollection: coll.getFullName(),
            toShard: shardNames[0],
        },
    },
    {
        name: "moveCollection",
        cmd: {
            moveCollection: coll.getFullName(),
            toShard: shardNames[1],
        },
        shardColl: false,
    },
    {
        name: "rewriteCollection",
        cmd: {
            rewriteCollection: coll.getFullName(),
            numInitialChunks: 1,
        },
    },
];

for (const cmdObj of cmdsToTest) {
    const name = cmdObj.name;
    const cmd = cmdObj.cmd;
    const shardColl = cmdObj.shardColl ?? true;
    describe(`${name} rejects collections with MongoDB Search indexes`, function () {
        before(setUp(shardColl));
        after(tearDown());

        it(`fails ${name} when a search index exists`, testWithExpectedFailure(cmd));
        it(`allows ${name} after dropping the search index`, testAfterDroppingIndex(cmd));
    });
}
