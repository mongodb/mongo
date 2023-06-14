// Test setClusterParameter-setFeatureCompatibilityVersion serialization.
// @tags: [
//  creates_and_authenticates_user,
//  requires_auth,
//  requires_non_retryable_commands,
//  requires_persistence,
//  requires_replication,
// ]

load("jstests/noPassthrough/libs/user_write_blocking.js");

(function() {
'use strict';

const {ReplicaFixture, ShardingFixture} = UserWriteBlockHelpers;

// Functions run in parallel shells to downgrade FCV and update a cluster parameter.
const parallelShellSetFCVFn = `({conn}) => {
    assert.commandWorked(
        conn.getDB('admin').runCommand({setFeatureCompatibilityVersion: "${lastLTSFCV}"}));
    }`;
const parallelShellSetCSPFnSuccess =
    `({conn}) => {
assert.commandWorked(
    conn.getDB('admin').runCommand({setClusterParameter: {testMinFcvClusterParameter: {intData: 106}}}));
}`;
const parallelShellSetCSPFnFail =
    `({conn}) => {
assert.commandFailedWithCode(
    conn.getDB('admin').runCommand({setClusterParameter: {testMinFcvClusterParameter: {intData: 106}}}), ErrorCodes.BadValue);
}`;

function runReplSetTest(fixture) {
    // Assert that the cluster parameter is initially at default value of 16.
    fixture.asAdmin(({admin}) => {
        const intData = assert
                            .commandWorked(admin.runCommand(
                                {getClusterParameter: "testMinFcvClusterParameter"}))
                            .clusterParameters[0]
                            .intData;
        assert.eq(intData, 16);
    });

    // Set a failpoint to make setFeatureCompatibilityVersion hang when modifying the FCV document.
    // setClusterParameter should also hang until setFCV unblocks.
    const hangSetFCVFailPoint = fixture.setFailPoint('hangBeforeUpdatingFcvDoc');

    // Run 2 parallel shells - one to downgrade FCV and another to set a cluster parameter.
    let hangSetFCVWaiter = fixture.runInParallelShell(true, parallelShellSetFCVFn);
    hangSetFCVFailPoint.wait();

    let hangSetCSPWaiter = fixture.runInParallelShell(true, parallelShellSetCSPFnFail);

    // Assert that the cluster parameter has been disabled.
    fixture.asAdmin(({admin}) => assert.commandFailedWithCode(
                        admin.runCommand({getClusterParameter: "testMinFcvClusterParameter"}),
                        ErrorCodes.BadValue));

    // Turn off the failpoint. This should unblock setFCV, allowing FCV downgrade.
    // setClusterParameter should also complete but return an error since the cluster parameter
    // is unavailable on the downgraded FCV.
    hangSetFCVFailPoint.off();
    hangSetFCVWaiter();
    hangSetCSPWaiter();

    // Verify that FCV upgrade succeeds when there are no ongoing setClusterParameter operations.
    // The cluster parameter should also reenable.
    fixture.asAdmin(({admin}) => assert.commandWorked(
                        admin.runCommand({setFeatureCompatibilityVersion: latestFCV})));
    fixture.asAdmin(({admin}) => {
        const intData = assert
                            .commandWorked(admin.runCommand(
                                {getClusterParameter: "testMinFcvClusterParameter"}))
                            .clusterParameters[0]
                            .intData;
        assert.eq(intData, 16);
    });

    // Now, set a failpoint to make setClusterParameter hang.
    const hangSetCSPFailPoint = fixture.setFailPoint('hangInSetClusterParameter');
    hangSetCSPWaiter = fixture.runInParallelShell(true, parallelShellSetCSPFnSuccess);
    hangSetCSPFailPoint.wait();

    hangSetFCVWaiter = fixture.runInParallelShell(true, parallelShellSetFCVFn);

    // Assert that FCV downgrade remains hung (Current FCV is still latestFCV) while
    // setClusterParameter is incomplete. This occurs due to contention for the FCV lock.
    fixture.assertFCV(latestFCV);

    // Turn off the failpoint and allow setClusterParameter to drain successfully.
    hangSetCSPFailPoint.off();
    hangSetCSPWaiter();
    hangSetFCVWaiter();

    // Assert that FCV downgrade proceeded normally after setCSP completed. The CSP should now be
    // disabled.
    fixture.assertFCV(lastLTSFCV);
    fixture.asAdmin(({admin}) => assert.commandFailedWithCode(
                        admin.runCommand({getClusterParameter: "testMinFcvClusterParameter"}),
                        ErrorCodes.BadValue));
}

function runShardedTest(fixture) {
    // Check that the starting value of testMinFcvClusterParameter is the default of 16.
    fixture.asAdmin(({admin}) => {
        const intData = assert
                            .commandWorked(admin.runCommand(
                                {getClusterParameter: "testMinFcvClusterParameter"}))
                            .clusterParameters[0]
                            .intData;
        assert.eq(intData, 16);
    });

    // Set a failpoint to make setFeatureCompatibilityVersion hang on a shard. setClusterParameter
    // should also complete but return an error since testMinFCVClusterParameter is unavailable on
    // the lower FCV.
    let hangSetFCVWaiter = fixture.hangTransition({setFeatureCompatibilityVersion: lastLTSFCV},
                                                  'hangBeforeUpdatingFcvDoc');
    fixture.asAdmin(
        ({admin}) => assert.commandFailedWithCode(
            admin.runCommand({setClusterParameter: {testMinFcvClusterParameter: {intData: 106}}}),
            ErrorCodes.BadValue));
    fixture.asAdmin(({admin}) => assert.commandFailedWithCode(
                        admin.runCommand({getClusterParameter: "testMinFcvClusterParameter"}),
                        ErrorCodes.BadValue));

    // Turn off the failpoint and wait for the hung FCV downgrade to complete.
    hangSetFCVWaiter.failpoint.off();
    hangSetFCVWaiter.waiter();

    // Assert that the FCV downgraded successfully and the cluster parameter remains disabled on
    // the config server due to the FCV downgrade.
    fixture.assertFCV(lastLTSFCV);
    fixture.asAdmin(({admin}) => assert.commandFailedWithCode(
                        admin.runCommand({getClusterParameter: "testMinFcvClusterParameter"}),
                        ErrorCodes.BadValue));

    // After the failpoint is turned off, FCV upgrade should occur normally.
    // testMinFcvClusterParameter should also become available.
    fixture.asAdmin(({admin}) => assert.commandWorked(
                        admin.runCommand({setFeatureCompatibilityVersion: latestFCV})));
    fixture.asAdmin(({admin}) => {
        const intData = assert
                            .commandWorked(admin.runCommand(
                                {getClusterParameter: "testMinFcvClusterParameter"}))
                            .clusterParameters[0]
                            .intData;
        assert.eq(intData, 16);
    });

    // Set a failpoint to make setClusterParameter hang on a shard. FCV downgrade should
    // fail while setClusterParameter is in progress.
    let hangSetClusterParameterWaiter =
        fixture.hangTransition({setClusterParameter: {testMinFcvClusterParameter: {intData: 107}}},
                               'hangInShardsvrSetClusterParameter');
    fixture.asAdmin(({admin}) => assert.commandFailedWithCode(
                        admin.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                        ErrorCodes.CannotDowngrade));

    // Turn off the failpoint and wait for the hung setClusterParameter operation to drain.
    hangSetClusterParameterWaiter.failpoint.off();
    hangSetClusterParameterWaiter.waiter();

    // Verify that the cluster parameter was correctly set and FCV remains at latestFCV.
    fixture.assertFCV(latestFCV);
    fixture.asAdmin(({admin}) => {
        const intData = assert
                            .commandWorked(admin.runCommand(
                                {getClusterParameter: "testMinFcvClusterParameter"}))
                            .clusterParameters[0]
                            .intData;
        assert.eq(intData, 107);
    });
}

{
    const rst = new ReplicaFixture();
    runReplSetTest(rst);
    rst.stop();
}

{
    const st = new ShardingFixture();
    runShardedTest(st);
    st.stop();
}
}());
