/**
 * Tests that resharding a timeseries collection with forceRedistribution works correctly
 * when using the same shard key.
 *
 * @tags: [
 *   requires_fcv_80,
 *   featureFlagReshardingForTimeseries,
 * ]
 */

import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const dbName = jsTestName();
const collName = "coll";
const ns = `${dbName}.${collName}`;

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;

const timeseriesInfo = {
    timeField: "ts",
    metaField: "metadata",
};

// This will be translated internally to {"meta.x": 1}.
const shardKeyPattern = {
    "metadata.x": 1,
};

const coll = reshardingTest.createShardedCollection({
    ns: ns,
    shardKeyPattern: shardKeyPattern,
    chunks: [
        {min: {"meta.x": MinKey}, max: {"meta.x": 0}, shard: donorShardNames[0]},
        {min: {"meta.x": 0}, max: {"meta.x": MaxKey}, shard: donorShardNames[1]},
    ],
    collOptions: {
        timeseries: timeseriesInfo,
    },
});

assert.commandWorked(
    coll.insert([
        {data: 1, ts: new Date(), metadata: {x: -1, y: -1}},
        {data: 2, ts: new Date(), metadata: {x: 1, y: 1}},
    ]),
);

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: shardKeyPattern,
        newChunks: [
            {min: {"meta.x": MinKey}, max: {"meta.x": 0}, shard: donorShardNames[0]},
            {min: {"meta.x": 0}, max: {"meta.x": MaxKey}, shard: donorShardNames[1]},
        ],
        forceRedistribution: true,
    },
    () => {},
);

reshardingTest.teardown();
