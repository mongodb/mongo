/**
 * Tests that when timeseries resharding is in building-index phase, abort the resharding operation
 * will exit correctly.
 *
 * Timeseries variant of resharding_abort_during_building_index.js.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */

import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const ns = "reshardingDb.coll";
const reshardingTest = new ReshardingTest({numDonors: 2, enableElections: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const timeseriesInfo = {timeField: "ts", metaField: "metaTest"};
const sourceCollection = reshardingTest.createShardedCollection({
    ns,
    shardKeyPattern: {"metaTest.x": 1},
    chunks: [
        {min: {"meta.x": MinKey}, max: {"meta.x": 0}, shard: donorShardNames[0]},
        {min: {"meta.x": 0}, max: {"meta.x": MaxKey}, shard: donorShardNames[1]},
    ],
    shardCollOptions: {timeseries: timeseriesInfo},
});
const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);

const recipientShardNames = reshardingTest.recipientShardNames;
const recipient = new Mongo(topology.shards[recipientShardNames[0]].primary);

assert.commandWorked(
    mongos.getCollection(ns).insert([
        {data: 1, ts: new Date(), metaTest: {x: 1, y: -1}},
        {data: 2, ts: new Date(), metaTest: {x: 2, y: -2}},
    ]),
);
assert.commandWorked(mongos.getCollection(ns).createIndex({"metaTest.x": 1}));
const hangAfterInitializingIndexBuildFailPoint = configureFailPoint(recipient, "hangAfterInitializingIndexBuild");

// In legacy timeseries (FCV < 9.0), config.reshardingOperations and abortReshardCollection
// use the bucket namespace. Compute it once before starting resharding.
const abortNs = getTimeseriesCollForDDLOps(
    mongos.getDB("reshardingDb"),
    mongos.getDB("reshardingDb").getCollection("coll"),
).getFullName();

let awaitAbort;
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {"metaTest.y": 1},
        newChunks: [{min: {"meta.y": MinKey}, max: {"meta.y": MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        reshardingTest.awaitCloneTimestampChosen();
        hangAfterInitializingIndexBuildFailPoint.wait();
        jsTestLog("Hang primary during building index, then abort resharding");

        assert.neq(null, mongos.getCollection("config.reshardingOperations").findOne({ns: abortNs}));
        awaitAbort = startParallelShell(
            funWithArgs(function (sourceNamespace) {
                db.adminCommand({abortReshardCollection: sourceNamespace});
            }, abortNs),
            mongos.port,
        );

        assert.soon(() => {
            const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({ns: abortNs});
            return coordinatorDoc === null || coordinatorDoc.state === "aborting";
        });
    },
    {expectedErrorCode: ErrorCodes.ReshardCollectionAborted},
);

awaitAbort();
hangAfterInitializingIndexBuildFailPoint.off();

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
