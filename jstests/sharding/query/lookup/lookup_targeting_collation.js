/**
 * Test that verifies that $lookup and $graphLookup obey the collection default collation when
 * targeting an untracked collection on the primary shard, but execute as a merger on a different
 * shard.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   requires_sharding,
 *   requires_spawning_own_processes,
 *   requires_profiling,
 * ]
 */

import {ShardTargetingTest} from "jstests/libs/shard_targeting_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kDbName = "lookup_targeting_collation";
const st = new ShardingTest({shards: 2, mongos: 1});
const db = st.s.getDB(kDbName);
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;
const shard0DB = st.shard0.getDB(kDbName);
const shard1DB = st.shard1.getDB(kDbName);

assert.commandWorked(db.adminCommand({enableSharding: kDbName, primaryShard: shard0}));

// Map from shard name to database connection. Used to determine which shard to read from when
// gathering profiler output.
const shardDBMap = {
    [shard0]: shard0DB,
    [shard1]: shard1DB,
};

const shardTargetingTest = new ShardTargetingTest(db, shardDBMap);

// Create an unsplittable collection on a shard that is NOT the primary.
const kUnsplittable1CollName = "unsplittable_1";
const kUnsplittable1Docs = [
    {_id: 0, a: "AA", unsplittable: 1},
    {_id: 1, a: "Aa", unsplittable: 1},
    {_id: 2, a: "aa", unsplittable: 1}
];
shardTargetingTest.setupColl({
    collName: kUnsplittable1CollName,
    docs: kUnsplittable1Docs,
    collType: "unsplittable",
    owningShard: shard1,
});

const kLookupStage = {
    $lookup: {from: kUnsplittable1CollName, localField: "a", foreignField: "a", as: "out"}
};
const kGraphLookupStage = {$graphLookup: {from: kUnsplittable1CollName, startWith: "$a", connectFromField: "a", connectToField: "a", as: "out"}};
for (const stage of [kLookupStage, kGraphLookupStage]) {
    const stageName = Object.keys(stage)[0];
    jsTestLog("Testing stage " + stageName);

    // Create an untracked collection on the primary shard with no collection-default collation.
    const kUntrackedCollName = "untracked";
    const kUntrackedDocs = [
        {_id: 0, a: "AA", untracked: 1},
        {_id: 1, a: "Aa", untracked: 1},
        {_id: 2, a: "aa", untracked: 1}
    ];
    assert.commandWorked(db.runCommand({create: kUntrackedCollName}));
    assert.commandWorked(db.runCommand({insert: kUntrackedCollName, documents: kUntrackedDocs}));

    const pipeline = [stage];
    let expectedResults = [
        {
            _id: 0,
            a: "AA",
            untracked: 1,
            out: [
                {_id: 0, a: "AA", unsplittable: 1},
            ]
        },
        {
            _id: 1,
            a: "Aa",
            untracked: 1,
            out: [
                {_id: 1, a: "Aa", unsplittable: 1},
            ]
        },
        {_id: 2, a: "aa", untracked: 1, out: [{_id: 2, a: "aa", unsplittable: 1}]}
    ];
    const profileFilters = {
        [shard0]: [{ns: kUntrackedCollName, expectedStages: []}],
        [shard1]: [{ns: kUntrackedCollName, expectedStages: ["$mergeCursors", stageName]}],
    };
    const explainObj = {
        expectedMergingShard: shard1,
        expectedMergingStages: ["$mergeCursors", stageName]
    };

    // Outer collection is untracked and lives on the primary shard. Inner collection is tracked and
    // unsplittable and lives on a shard that is not the primary shard. We should merge on the shard
    // which owns the unsplittable collection and use the simple collation.
    shardTargetingTest.assertShardTargeting({
        pipeline: pipeline,
        targetCollName: kUntrackedCollName,
        explainAssertionObj: explainObj,
        comment: "lookup_untracked_to_unsplittable_without_collation",
        expectedResults: expectedResults,
        profileFilters: profileFilters,
    });

    // Drop and recreate the untracked collection on the primary shard, this time with a non-simple
    // collation.
    const kCaseInsensitive = {locale: "en_US", strength: 2};
    assert.commandWorked(db.runCommand({drop: kUntrackedCollName}));
    assert.commandWorked(db.runCommand({create: kUntrackedCollName, collation: kCaseInsensitive}));
    assert.commandWorked(db.runCommand({insert: kUntrackedCollName, documents: kUntrackedDocs}));

    // The targeting should not change, but the results should.
    expectedResults = [
        {
            _id: 0,
            a: "AA",
            untracked: 1,
            out: [
                {_id: 0, a: "AA", unsplittable: 1},
                {_id: 1, a: "Aa", unsplittable: 1},
                {_id: 2, a: "aa", unsplittable: 1}
            ]
        },
        {
            _id: 1,
            a: "Aa",
            untracked: 1,
            out: [
                {_id: 0, a: "AA", unsplittable: 1},
                {_id: 1, a: "Aa", unsplittable: 1},
                {_id: 2, a: "aa", unsplittable: 1}
            ]
        },
        {
            _id: 2,
            a: "aa",
            untracked: 1,
            out: [
                {_id: 0, a: "AA", unsplittable: 1},
                {_id: 1, a: "Aa", unsplittable: 1},
                {_id: 2, a: "aa", unsplittable: 1}
            ]
        }
    ];

    // Outer collection is untracked and lives on the primary shard. Inner collection is tracked and
    // unsplittable and lives on a shard that is not the primary shard. We should still merge on the
    // shard which owns the unsplittable collection, but this time, we should use the untracked
    // collection's default collation when merging.
    shardTargetingTest.assertShardTargeting({
        pipeline: pipeline,
        targetCollName: kUntrackedCollName,
        explainAssertionObj: explainObj,
        comment: "lookup_untracked_to_unsplittable_with_collation",
        expectedResults: expectedResults,
        profileFilters: profileFilters,
    });
    assert.commandWorked(db.runCommand({drop: kUntrackedCollName}));
}
st.stop();
