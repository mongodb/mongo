/*
 * Tests that during a chunk migration, a $search stage using stored source within a $unionWith or
 * $lookup subpipeline will check the shard version in order to get up-to-date routing information,
 * even though it will not need to read from mongot (since the stored source documents come from
 * mongot).
 *
 * This test is only run in the mongot_e2e_sharded_cluster suite since it requires beginning with
 * unsharded collections and then manually triggering the chunk migration.
 *
 * @tags: [ requires_sharding, assumes_unsharded_collection ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {getShardNames} from "jstests/libs/sharded_cluster_fixture_helpers.js";

const testDb = db.getSiblingDB(jsTestName());
const shardNames = getShardNames(testDb.getMongo());
assert.gte(shardNames.length, 2, "Test requires at least 2 shards");
const primaryShardName = shardNames[0];
const otherShardName = shardNames[1];

assert.commandWorked(
    db.adminCommand({enableSharding: testDb.getName(), primaryShard: primaryShardName}));

const innerCollName = "innerColl";
const searchIndexName = "excludeStateBirdIndex";
const searchStageDefinition = {
    index: searchIndexName,
    wildcard: {
        query: "*",  // This matches all documents
        path: "state",
        allowAnalyzedField: true,
    },
    returnStoredSource: true
};

function testSearchStoredSourceInSubpipeline(pipeline, expectedResults) {
    // Outer collection is unsharded.
    const outerColl = testDb.outerColl;
    outerColl.drop();
    assert.commandWorked(outerColl.insertMany([
        {
            _id: 1,
            state: "NY",
        },
        {
            _id: 3,
            state: "Hawaii",
        },
        {
            _id: 5,
            state: "NJ",
        },
    ]));

    // Inner collection is unsharded to start.
    const innerColl = testDb.getCollection(innerCollName);
    innerColl.drop();
    assert.commandWorked(innerColl.insertMany([
        {
            _id: 10000,
            state: "NY",
            governor: "Kathy Hochul",
            facts: {state_bird: "Eastern Bluebird", most_popular_sandwich: "Pastrami on Rye"}
        },
        {
            _id: 1,
            state: "Hawaii",
            governor: "Josh Green",
            facts: {state_bird: "Nene", most_popular_sandwich: "Kalua Pork"}
        }
    ]));

    // Shard the inner collection and move a chunk (with _id: 10000) to the other shard.
    assert.commandWorked(
        testDb.adminCommand({shardCollection: innerColl.getFullName(), key: {_id: 1}}));
    assert.commandWorked(testDb.adminCommand({split: innerColl.getFullName(), middle: {_id: 3}}));
    assert.commandWorked(testDb.adminCommand({
        moveChunk: innerColl.getFullName(),
        find: {_id: 3},
        to: otherShardName,
        _waitForDelete: true  // We need to wait for delete, or we are going to read orphans
    }));

    const searchIndexDefinition = {
        mappings: {dynamic: true},
        storedSource: {exclude: ["facts.state_bird"]}
    };
    createSearchIndex(innerColl, {name: searchIndexName, definition: searchIndexDefinition});

    assertArrayEq({actual: outerColl.aggregate(pipeline).toArray(), expected: expectedResults});

    dropSearchIndex(innerColl, {name: searchIndexName});
}

// Test $search under a $unionWith.
let pipeline = [{
    $unionWith: {
        coll: innerCollName,
        pipeline: [
            {$search: searchStageDefinition},
            {$project: {_id: 0}},
        ]
    }
}];
let expectedResults = [
    {
        _id: 1,
        state: "NY",
    },
    {
        _id: 3,
        state: "Hawaii",
    },
    {
        _id: 5,
        state: "NJ",
    },
    {
        state: "Hawaii",
        governor: "Josh Green",
        facts: {most_popular_sandwich: "Kalua Pork"},
    },
    {
        state: "NY",
        governor: "Kathy Hochul",
        facts: {most_popular_sandwich: "Pastrami on Rye"},
    },
];
testSearchStoredSourceInSubpipeline(pipeline, expectedResults);

// Test $search under a $lookup.
pipeline = [{
    $lookup: {
        from: innerCollName,
        localField: "state",
        foreignField: "state",
        pipeline: [
            {
                $search: searchStageDefinition
            },
            {$project: {_id: 0}},
        ],
        as: "matchedDocs"
    }
}];
expectedResults = [
    {
        _id: 1,
        state: "NY",
        matchedDocs: [{
            state: "NY",
            governor: "Kathy Hochul",
            facts: {most_popular_sandwich: "Pastrami on Rye"},
        }]
    },
    {
        _id: 3,
        state: "Hawaii",
        matchedDocs: [{
            state: "Hawaii",
            governor: "Josh Green",
            facts: {most_popular_sandwich: "Kalua Pork"},
        }]
    },
    {_id: 5, state: "NJ", matchedDocs: []}
];

testSearchStoredSourceInSubpipeline(pipeline, expectedResults);
