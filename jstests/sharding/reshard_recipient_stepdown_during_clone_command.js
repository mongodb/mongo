/**
 * Regression test for BF-42600: deadlock between a stepdown and the session held by
 * ShardsvrReshardRecipientClone. The clone command is sent with OSI (lsid + txnNum) under
 * featureFlagReshardingInitNoRefresh, causing the command to check out the
 * session for the lifetime of the command. When the node steps down, the stepdown blocks
 * waiting for that session while the clone command blocks in .get() ignoring the opCtx kill.
 *
 * @tags: [
 *   requires_fcv_90,
 *   uses_resharding,
 *   featureFlagReshardingInitNoRefresh,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({
    numDonors: 1,
    numRecipients: 1,
    enableElections: true,
    minimumOperationDurationMS: 0,
});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

const mongos = sourceCollection.getMongo();
assert.commandWorked(
    mongos.getCollection("reshardingDb.coll").insert([
        {oldKey: 1, newKey: -1},
        {oldKey: 2, newKey: -2},
    ]),
);

const recipientPrimary = reshardingTest.getReplSetForShard(recipientShardNames[0]).getPrimary();
const recipientPrimaryHost = recipientPrimary.host;

// Pause the clone command, ensuring the session remains checked out.
const cloneCmdFp = configureFailPoint(recipientPrimary, "pauseAfterRecipientReceiveCloneCmd");

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    () => {
        cloneCmdFp.wait();
        jsTestLog("Clone command paused; session is checked out on recipient.");

        const stepdownThread = new Thread(function (host) {
            try {
                new Mongo(host).adminCommand({replSetStepDown: 60, force: true, secondaryCatchUpPeriodSecs: 0});
            } catch (e) {}
        }, recipientPrimaryHost);
        stepdownThread.start();

        // Wait until the stepdown has committed (isWritablePrimary=false) before releasing
        // the failpoint. This ensures the deadlock condition is set up: the stepdown is
        // blocked on the session held by the clone command.
        assert.soonRetryOnNetworkErrors(
            () => !assert.commandWorked(recipientPrimary.adminCommand({hello: 1})).isWritablePrimary,
            "Timed out waiting for recipient stepdown to commit",
        );
        jsTestLog("Stepdown committed; releasing clone failpoint.");

        // The clone command now reaches .get() with an already-killed opCtx.
        cloneCmdFp.off();

        assert.soonRetryOnNetworkErrors(() => {
            const status = assert.commandWorked(recipientPrimary.adminCommand({replSetGetStatus: 1}));
            jsTestLog("Recipient myState: " + status.myState);
            return status.myState !== 1;
        }, "DEADLOCK: recipient is permanently stuck as PRIMARY.");

        jsTestLog("Stepdown completed. No deadlock.");
        stepdownThread.join();
    },
);

reshardingTest.teardown();
