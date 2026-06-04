/**
 * Tests that timeseries resharding can successfully handle an abort request after the coordinator
 * is in state preparing-to-donate but before it has flushed its state change to prompt participant
 * state machine creation. See SERVER-56936 for more details.
 *
 * Timeseries variant of resharding_abort_in_preparing_to_donate.js.
 *
 * @tags: [
 *   uses_atclustertime,
 *   requires_fcv_80,
 * ]
 */
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const originalCollectionNs = "reshardingDb.coll";

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const timeseriesInfo = {timeField: "ts", metaField: "metaTest"};
const sourceCollection = reshardingTest.createShardedCollection({
    ns: originalCollectionNs,
    shardKeyPattern: {"metaTest.x": 1},
    chunks: [
        {min: {"meta.x": MinKey}, max: {"meta.x": 0}, shard: donorShardNames[0]},
        {min: {"meta.x": 0}, max: {"meta.x": MaxKey}, shard: donorShardNames[1]},
    ],
    shardCollOptions: {timeseries: timeseriesInfo},
});

const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const configsvr = new Mongo(topology.configsvr.nodes[0]);

const pauseAfterPreparingToDonateFP = configureFailPoint(configsvr, "reshardingPauseCoordinatorAfterPreparingToDonate");

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
        newChunks: [
            {min: {"meta.y": MinKey}, max: {"meta.y": 0}, shard: recipientShardNames[0]},
            {min: {"meta.y": 0}, max: {"meta.y": MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    () => {
        pauseAfterPreparingToDonateFP.wait();
        assert.neq(
            null,
            mongos.getCollection("config.reshardingOperations").findOne({
                ns: abortNs,
            }),
        );
        // Signaling abort will cause the
        // pauseAfterPreparingToDonateFP to throw, implicitly
        // allowing the coordinator to make progress without
        // explicitly turning off the failpoint.
        awaitAbort = startParallelShell(
            funWithArgs(function (sourceNamespace) {
                db.adminCommand({abortReshardCollection: sourceNamespace});
            }, abortNs),
            mongos.port,
        );
        // Wait for the coordinator to remove coordinator document from config.reshardingOperations
        // as a result of the recipients and donors transitioning to done due to abort.
        assert.soon(() => {
            const coordinatorDoc = mongos.getCollection("config.reshardingOperations").findOne({
                ns: abortNs,
            });
            return coordinatorDoc === null || coordinatorDoc.state === "aborting";
        });
    },
    {expectedErrorCode: ErrorCodes.ReshardCollectionAborted},
);

awaitAbort();
pauseAfterPreparingToDonateFP.off();

// Verify the translation path using the internal field name (meta.x).
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
