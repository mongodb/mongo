/**
 * Tests that range deletion tasks for a collection actively undergoing resharding are not executed.
 *
 * @tags: [
 *  requires_fcv_81,
 *  featureFlagReshardingRegistry,
 * ]
 */

import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const dbName = jsTestName();
const controlCollName = "controlColl";
const reshardingCollName = "reshardingColl";
const controlCollNs = `${dbName}.${controlCollName}`;
const reshardingCollNs = `${dbName}.${reshardingCollName}`;

const [reshardingSplitValue, migrationSplitValue, numDocs] = [2, 5, 10];
assert(0 < reshardingSplitValue, "reshardingSplitValue must be > 0");
assert(reshardingSplitValue < migrationSplitValue, "reshardingSplitValue must be < migrationSplitValue");
assert(migrationSplitValue < numDocs, "migrationSplitValue must be < numDocs");

function seedCollection(coll) {
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({oldKey: i, newKey: i});
    }
    assert.commandWorked(bulk.execute());
}

function runTest(shouldAbort) {
    const reshardingTest = new ReshardingTest({numDonors: 2});
    reshardingTest.setup();

    const donorShardName = reshardingTest.donorShardNames[0];
    const recipientShardName = reshardingTest.recipientShardNames[0];

    // create controlColl first because createShardedCollection clobbers
    // ReshardingTest's `ns` instance variable
    const controlColl = reshardingTest.createShardedCollection({
        ns: controlCollNs,
        shardKeyPattern: {oldKey: 1},
        chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardName}],
        primaryShardName: donorShardName,
    });
    const reshardingColl = reshardingTest.createShardedCollection({
        ns: reshardingCollNs,
        shardKeyPattern: {oldKey: 1},
        chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardName}],
        primaryShardName: donorShardName,
    });
    const originalReshardingCollUUID = reshardingTest.sourceCollectionUUID;

    seedCollection(controlColl);
    seedCollection(reshardingColl);

    const s = reshardingTest.getMongos();
    const topology = DiscoverTopology.findConnectedNodes(s);
    const donorShard = new Mongo(topology.shards[donorShardName].primary);
    assert.commandWorked(donorShard.adminCommand({setParameter: 1, orphanCleanupDelaySecs: 0}));
    assert.commandWorked(donorShard.adminCommand({setParameter: 1, disableResumableRangeDeleter: true}));
    // Range Deleter: disabled, Resharding: inactive (unscheduled)

    // because of resharding text fixtures post operation consistency checks, if we
    // abort resharding, we can't have any docs on any recipients, but if we succeed,
    // we can't have any docs on any donors
    const moveChunkRecipientName = shouldAbort ? reshardingTest.donorShardNames[1] : recipientShardName;
    const moveChunk = (ns) =>
        assert.commandWorked(
            s.adminCommand({
                moveChunk: ns,
                find: {oldKey: migrationSplitValue},
                to: moveChunkRecipientName,
                _waitForDelete: false,
            }),
        );
    // moveChunk for reshardingColl first so the range deleter pauses before working on controlColl
    moveChunk(reshardingCollNs);
    moveChunk(controlCollNs);

    const assertRangeDeletionTaskPresent = (ns, expected) => {
        const rangeDeletions = donorShard.getDB("config").rangeDeletions;
        assert.soon(
            () => {
                const task = rangeDeletions.findOne({nss: ns});
                return !!task == expected;
            },
            "Expected range deletion task to be " +
                (expected ? "present" : "null") +
                " for namespace " +
                ns +
                " but found config.rangeDeletions: " +
                tojson(rangeDeletions.find().toArray()),
        );
    };

    assertRangeDeletionTaskPresent(controlCollNs, true);
    assertRangeDeletionTaskPresent(reshardingCollNs, true);

    const assertNumOrphansEquals = (collName, expected) => {
        const donorColl = donorShard.getDB(dbName).getCollection(collName);
        const numOrphans = donorColl.countDocuments({oldKey: {$gte: migrationSplitValue}});
        assert.eq(
            numOrphans,
            expected,
            `actual numOrphans on donor shard does not match expected for ns ${dbName}.${collName}`,
        );
    };
    const expectedInitialOrphans = numDocs - migrationSplitValue;
    assertNumOrphansEquals(reshardingCollName, expectedInitialOrphans);
    assertNumOrphansEquals(controlCollName, expectedInitialOrphans);

    const recipientShard = new Mongo(topology.shards[recipientShardName].primary);
    const hangAfterInitializingIndexBuildFailPoint = configureFailPoint(
        recipientShard,
        "hangAfterInitializingIndexBuild",
    );
    let awaitAbort;
    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: {newKey: 1},
            newChunks: [
                {min: {newKey: MinKey}, max: {newKey: reshardingSplitValue}, shard: donorShardName},
                {min: {newKey: reshardingSplitValue}, max: {newKey: MaxKey}, shard: recipientShardName},
            ],
        },
        () => {
            // RangeDeleter: disabled, Resharding: Active
            hangAfterInitializingIndexBuildFailPoint.wait();
            assert.neq(null, s.getCollection("config.reshardingOperations").findOne({ns: reshardingCollNs}));

            assert.commandWorked(donorShard.adminCommand({setParameter: 1, disableResumableRangeDeleter: false}));
            // RangeDeleter: Enabled, Resharding: Active (paused)

            assertRangeDeletionTaskPresent(controlCollNs, false);
            assertRangeDeletionTaskPresent(reshardingCollNs, true);

            assertNumOrphansEquals(controlCollName, 0);
            assertNumOrphansEquals(reshardingCollName, expectedInitialOrphans);

            if (shouldAbort) {
                awaitAbort = startParallelShell(
                    funWithArgs(function (sourceNamespace) {
                        db.adminCommand({abortReshardCollection: sourceNamespace});
                    }, reshardingCollNs),
                    s.port,
                );
            } else {
                hangAfterInitializingIndexBuildFailPoint.off();
            }
        },
        {expectedErrorCode: shouldAbort ? ErrorCodes.ReshardCollectionAborted : ErrorCodes.OK},
    );

    if (shouldAbort) {
        awaitAbort();
        hangAfterInitializingIndexBuildFailPoint.off();

        const finalUUID = reshardingTest.sourceCollectionUUID;
        assert.eq(originalReshardingCollUUID, finalUUID);
    }
    // RangeDeleter: Enabled, Resharding: Inactive (completed)

    // If resharding was aborted, then the range deleter should pick up and handle the paused task.
    // If resharding succeeded, then the original collection uuid will have been dropped, and the
    // range deleter will discard the task. Either way, there should be no task and no orphans:
    assertRangeDeletionTaskPresent(reshardingCollNs, false);
    assertNumOrphansEquals(reshardingCollName, 0);

    reshardingTest.teardown();
}

runTest(true);
runTest(false);
