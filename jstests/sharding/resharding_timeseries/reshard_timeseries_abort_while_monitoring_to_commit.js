/**
 * Test that the timeseries resharding operation is aborted if any of the recipient shards
 * encounters an error during the Applying phase. Uses a non-"meta" metaField name to exercise
 * the shard key translation path (user-facing field -> internal bucket field).
 *
 * Timeseries variant of resharding_abort_while_monitoring_to_commit.js.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 1});
reshardingTest.setup();

const collNs = "reshardingDb.coll";
const donorShardNames = reshardingTest.donorShardNames;
const timeseriesInfo = {timeField: "ts", metaField: "metaTest"};
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {"metaTest.x": 1},
    chunks: [
        {min: {"meta.x": MinKey}, max: {"meta.x": 0}, shard: donorShardNames[0]},
        {min: {"meta.x": 0}, max: {"meta.x": MaxKey}, shard: donorShardNames[1]},
    ],
    shardCollOptions: {timeseries: timeseriesInfo},
});

const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);

const donor0 = new Mongo(topology.shards[donorShardNames[0]].primary);

const recipientShardNames = reshardingTest.recipientShardNames;
const recipient = new Mongo(topology.shards[recipientShardNames[0]].primary);

// We have the recipient shard fail the _shardsvrReshardingOperationTime command to verify the
// ReshardingCoordinator can successfully abort the resharding operation even when the commit
// monitor doesn't see the recipient shard as being caught up enough to engage the critical section
// on the donor shards.
const shardsvrReshardingOperationTimeFailpoint = configureFailPoint(recipient, "failCommand", {
    failInternalCommands: true,
    errorCode: ErrorCodes.Interrupted,
    failCommands: ["_shardsvrReshardingOperationTime"],
});

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {"metaTest.y": 1},
        newChunks: [
            {min: {"meta.y": MinKey}, max: {"meta.y": MaxKey}, shard: recipientShardNames[0]},
        ],
    },
    () => {
        // We wait until cloneTimestamp has been chosen to guarantee that any subsequent writes will
        // be applied by the ReshardingOplogApplier.
        reshardingTest.awaitCloneTimestampChosen();

        // We connect directly to one of the donor shards to perform an operations which will later
        // cause the recipient shard to error during its resharding oplog application. Connecting
        // directly to the shard bypasses any synchronization which might otherwise occur from the
        // Sharding DDL Coordinator.
        const donor0Collection = donor0.getCollection(sourceCollection.getFullName());
        assert.commandWorked(donor0Collection.runCommand("collMod"));
    },
    {
        expectedErrorCode: ErrorCodes.OplogOperationUnsupported,
    },
);

shardsvrReshardingOperationTimeFailpoint.off();

// After abort, the shard key in config.collections uses the internal field name (meta.x),
// not the user-facing metaField name (metaTest.x), verifying the translation path.
// config.collections is keyed by the bucket namespace for viewful timeseries (FCV < 9.0) and
// by the user-facing namespace for viewless timeseries (FCV >= 9.0).
const configNs = getTimeseriesCollForDDLOps(
    mongos.getDB("reshardingDb"),
    mongos.getDB("reshardingDb").getCollection("coll"),
).getFullName();
const collEntry = mongos.getCollection("config.collections").findOne({_id: configNs});
assert.neq(null, collEntry);
assert.docEq({"meta.x": 1}, collEntry.key);

reshardingTest.teardown();
