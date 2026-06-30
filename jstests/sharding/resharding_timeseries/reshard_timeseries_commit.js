/**
 * Test the commitReshardCollection command on a timeseries collection.
 *
 * Verifies commitReshardCollection accepts the user-facing timeseries namespace.
 *
 * Timeseries variant of resharding_commit.js.
 *
 * @tags: [
 *   uses_atclustertime,
 *   requires_fcv_80,
 *   multiversion_incompatible,
 * ]
 */
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const sourceNs = "reshardingDb.coll";

const reshardingTest = new ReshardingTest({
    numDonors: 2,
    numRecipients: 2,
    reshardInPlace: true,
    commitImplicitly: false,
});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const timeseriesInfo = {timeField: "ts", metaField: "metaTest"};

const inputCollection = reshardingTest.createShardedCollection({
    ns: sourceNs,
    // "metaTest.x" is the user-facing field; translated internally to {"meta.x": 1}.
    shardKeyPattern: {"metaTest.x": 1},
    chunks: [
        {min: {"meta.x": MinKey}, max: {"meta.x": 0}, shard: donorShardNames[0]},
        {min: {"meta.x": 0}, max: {"meta.x": MaxKey}, shard: donorShardNames[1]},
    ],
    shardCollOptions: {timeseries: timeseriesInfo},
});

const mongos = inputCollection.getMongo();

reshardingTest.withReshardingInBackground(
    {
        // "metaTest.y" is the user-facing field; translated internally to {"meta.y": 1}.
        newShardKeyPattern: {"metaTest.y": 1},
        newChunks: [
            {min: {"meta.y": MinKey}, max: {"meta.y": 0}, shard: recipientShardNames[0]},
            {min: {"meta.y": 0}, max: {"meta.y": MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    () => {
        reshardingTest.awaitCloneTimestampChosen();
        assert.commandWorked(mongos.adminCommand({commitReshardCollection: sourceNs}));
    },
);

// config.collections is keyed by the bucket namespace for viewful timeseries and the user-facing
// namespace for viewless timeseries. The stored shard key uses the internal field (meta.y).
const configNs = getTimeseriesCollForDDLOps(
    mongos.getDB("reshardingDb"),
    mongos.getDB("reshardingDb").getCollection("coll"),
).getFullName();
const collEntry = mongos.getCollection("config.collections").findOne({_id: configNs});
assert.neq(null, collEntry);
assert.docEq({"meta.y": 1}, collEntry.key);

reshardingTest.teardown();
