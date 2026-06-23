/**
 * Tests that resharding pre-commit verification respects the critical-section time budget.
 *
 * When the change-stream monitor (CSM) on a donor or recipient does not complete before the
 * configured share of the critical-section time remaining when strict consistency is reached
 * elapses, verification is skipped and the resharding operation still commits successfully
 * (rather than hanging on the stalled monitor).
 *
 * @tags: [
 *   requires_fcv_90,
 *   uses_resharding,
 *   featureFlagReshardingVerification,
 *   # This test sets failpoints on shard primaries so it is not compatible with stepdowns.
 *   does_not_support_stepdowns,
 *   # resharding can't run during FCV transitions
 *   cannot_run_during_upgrade_downgrade,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const kCsmStallFailpoint = "hangReshardingChangeStreamsMonitorBeforeStarting";
// Exceeds the critical section deadline so the timeout is what ends the coordinator's wait for CSM on donor and recipients.
const kFetchHangTimeMS = 6 * 1000;

function setVerificationTimeoutPercent(reshardingTest, percent) {
    reshardingTest._st.forEachConfigServer((configServer) => {
        assert.commandWorked(
            configServer.adminCommand({
                setParameter: 1,
                reshardingVerificationDeltaWaitRemainingCriticalSectionPercent: percent,
            }),
        );
    });
}

function createSourceCollection(reshardingTest) {
    const donorShardNames = reshardingTest.donorShardNames;
    const sourceCollection = reshardingTest.createShardedCollection({
        ns: "reshardingDb.coll_" + Math.floor(Math.random() * 1e9),
        shardKeyPattern: {oldKey: 1},
        chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
    });
    const docs = [];
    for (let i = 0; i < 20; i++) {
        docs.push({oldKey: i, newKey: -i});
    }
    assert.commandWorked(sourceCollection.insert(docs));
    return sourceCollection;
}

function runReshardingWithFailpoint(reshardingTest, failpoint) {
    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: {newKey: 1},
            newChunks: [
                {
                    min: {newKey: MinKey},
                    max: {newKey: MaxKey},
                    shard: reshardingTest.recipientShardNames[0],
                },
            ],
            performVerification: true,
        },
        () => {
            // Confirm the failpoint was actually hit before resharding proceeds. Otherwise the
            // test could pass for the wrong reason (e.g., the monitor never started, so there was
            // nothing for the deadline to interrupt).
            if (failpoint !== undefined) {
                failpoint.wait();
            }
        },
    );
}

const reshardingTest = new ReshardingTest({
    numDonors: 1,
    numRecipients: 1,
    reshardInPlace: false,
    minimumOperationDurationMS: 0,
    criticalSectionTimeoutMS: 5000,
});
reshardingTest.setup();

const kDefaultVerificationPercent = assert.commandWorked(
    reshardingTest._st.configRS.getPrimary().adminCommand({
        getParameter: 1,
        reshardingVerificationDeltaWaitRemainingCriticalSectionPercent: 1,
    }),
).reshardingVerificationDeltaWaitRemainingCriticalSectionPercent;

jsTestLog(
    "Stall the donor's change-streams monitor and verify resharding still commits because " +
        "verification times out at the deadline rather than hanging.",
);
{
    createSourceCollection(reshardingTest);

    const donorPrimary = reshardingTest
        .getReplSetForShard(reshardingTest.donorShardNames[0])
        .getPrimary();
    const failpoint = configureFailPoint(donorPrimary, kCsmStallFailpoint);

    // Give verification only 1% of the critical-section budget so the deadline trips quickly.
    setVerificationTimeoutPercent(reshardingTest, 1);

    runReshardingWithFailpoint(reshardingTest, failpoint);

    failpoint.off();
    setVerificationTimeoutPercent(reshardingTest, kDefaultVerificationPercent);
}

jsTestLog(
    "Stall the recipient's change-streams monitor and verify resharding still commits because " +
        "verification times out at the deadline rather than hanging.",
);
{
    createSourceCollection(reshardingTest);

    const recipientPrimary = reshardingTest
        .getReplSetForShard(reshardingTest.recipientShardNames[0])
        .getPrimary();
    const failpoint = configureFailPoint(recipientPrimary, kCsmStallFailpoint);

    setVerificationTimeoutPercent(reshardingTest, 1);

    runReshardingWithFailpoint(reshardingTest, failpoint);

    failpoint.off();
    setVerificationTimeoutPercent(reshardingTest, kDefaultVerificationPercent);
}

jsTestLog(
    "Make the donor's final-collection-stats command hang so the coordinator never gets a " +
        "response. Resharding should still commit because the coordinator-side verification " +
        "deadline bounds the wait.",
);
{
    createSourceCollection(reshardingTest);

    const donorPrimary = reshardingTest
        .getReplSetForShard(reshardingTest.donorShardNames[0])
        .getPrimary();
    const failpoint = configureFailPoint(donorPrimary, "failCommand", {
        failCommands: ["_shardsvrReshardingDonorFetchFinalCollectionStats"],
        blockConnection: true,
        blockTimeMS: kFetchHangTimeMS,
        failInternalCommands: true,
    });

    setVerificationTimeoutPercent(reshardingTest, 1);

    runReshardingWithFailpoint(reshardingTest);

    failpoint.off();
    setVerificationTimeoutPercent(reshardingTest, kDefaultVerificationPercent);
}

jsTestLog(
    "Make the recipient's final-collection-stats command hang so the coordinator never gets a " +
        "response. Resharding should still commit because the coordinator-side verification " +
        "deadline bounds the wait.",
);
{
    createSourceCollection(reshardingTest);

    const recipientPrimary = reshardingTest
        .getReplSetForShard(reshardingTest.recipientShardNames[0])
        .getPrimary();
    const failpoint = configureFailPoint(recipientPrimary, "failCommand", {
        failCommands: ["_shardsvrReshardingRecipientFetchFinalCollectionStats"],
        blockConnection: true,
        blockTimeMS: kFetchHangTimeMS,
        failInternalCommands: true,
    });

    setVerificationTimeoutPercent(reshardingTest, 1);

    runReshardingWithFailpoint(reshardingTest);

    failpoint.off();
    setVerificationTimeoutPercent(reshardingTest, kDefaultVerificationPercent);
}

jsTestLog(
    "With the verification timeout percent set to 0, the deadline equals the time strict " +
        "consistency is reached, so the delta wait is skipped immediately. The stalled " +
        "change-streams monitor's RPC should be interrupted immediately and resharding should " +
        "still commit.",
);
{
    createSourceCollection(reshardingTest);

    const donorPrimary = reshardingTest
        .getReplSetForShard(reshardingTest.donorShardNames[0])
        .getPrimary();
    const failpoint = configureFailPoint(donorPrimary, kCsmStallFailpoint);

    setVerificationTimeoutPercent(reshardingTest, 0);

    runReshardingWithFailpoint(reshardingTest, failpoint);

    failpoint.off();
    setVerificationTimeoutPercent(reshardingTest, kDefaultVerificationPercent);
}

jsTestLog(
    "Make the donor's final-collection-stats command return a non-retriable error. " +
        "Resharding should still commit since delta fetch failures are non-fatal.",
);
{
    createSourceCollection(reshardingTest);

    const donorPrimary = reshardingTest
        .getReplSetForShard(reshardingTest.donorShardNames[0])
        .getPrimary();
    const failpoint = configureFailPoint(donorPrimary, "failCommand", {
        failCommands: ["_shardsvrReshardingDonorFetchFinalCollectionStats"],
        errorCode: ErrorCodes.OperationFailed,
        failInternalCommands: true,
    });

    runReshardingWithFailpoint(reshardingTest);

    failpoint.off();
}

reshardingTest.teardown();
