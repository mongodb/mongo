// Test setUserWriteBlockMode command.
//
// @tags: [
//   creates_and_authenticates_user,
//   requires_auth,
//   requires_fcv_60,
//   requires_non_retryable_commands,
//   requires_persistence,
//   requires_replication,
//   disabled_for_fcv_6_1_upgrade,
// ]

load("jstests/noPassthrough/libs/user_write_blocking.js");

(function() {
'use strict';

const {
    WriteBlockState,
    ShardingFixture,
    ReplicaFixture,
    bypassUser,
    noBypassUser,
    password,
    keyfile
} = UserWriteBlockHelpers;

function runTest(fixture) {
    // While the cluster is in write block mode, FCV cannot be downgraded
    fixture.asAdmin(({admin}) => assert.commandWorked(
                        admin.runCommand({setUserWriteBlockMode: 1, global: true})));
    fixture.asAdmin(
        ({admin}) => assert.commandFailedWithCode(
            admin.runCommand({setFeatureCompatibilityVersion: "5.0"}), ErrorCodes.CannotDowngrade));
    fixture.assertWriteBlockMode(WriteBlockState.ENABLED);

    // When write block mode is disabled, FCV can be downgraded
    fixture.asAdmin(({admin}) => assert.commandWorked(
                        admin.runCommand({setUserWriteBlockMode: 1, global: false})));
    fixture.asAdmin(({admin}) => assert.commandWorked(
                        admin.runCommand({setFeatureCompatibilityVersion: "5.0"})));

    // While in a downgraded state, it is not possible to enable write blocking
    fixture.asAdmin(({admin}) => assert.commandFailed(
                        admin.runCommand({setUserWriteBlockMode: 1, global: true})));

    // When a cluster is started in a downgraded state, it's possible to upgrade FCV,
    // and block writes on pre-existing connections without the bypass privilege
    fixture.restart();
    fixture.asUser(({coll}) => assert.commandWorked(coll.insert({data: 1})));
    fixture.asAdmin(({admin}) => assert.commandWorked(
                        admin.runCommand({setFeatureCompatibilityVersion: "6.0"})));
    fixture.asUser(({coll}) => assert.commandWorked(coll.insert({data: 2})));
    fixture.asAdmin(({admin}) => assert.commandWorked(
                        admin.runCommand({setUserWriteBlockMode: 1, global: true})));
    fixture.asUser(({coll}) => assert.commandFailed(coll.insert({data: 3})));
    fixture.asAdmin(({coll}) => assert.commandWorked(coll.insert({data: 4})));

    // While the cluster is in the middle of activating write block mode, FCV cannot be downgraded.
    // This does not need to be tested on replicasets. FCV locks are uninterruptable. Waiting on a
    // failpoint on a replicaset primary would prevent FCV from ever completing.
    if (fixture.hangTransition) {
        let hangWaiter = fixture.hangTransition({setUserWriteBlockMode: 1, global: true},
                                                "hangInShardsvrSetUserWriteBlockMode");

        fixture.asAdmin(({admin}) => assert.commandFailedWithCode(
                            admin.runCommand({setFeatureCompatibilityVersion: "5.0"}),
                            ErrorCodes.CannotDowngrade));

        // Restart the config server primary while in the middle of activating write block mode. FCV
        // downgrade should still fail.
        fixture.restartConfigPrimary();
        fixture.asAdmin(({admin}) => assert.commandFailedWithCode(
                            admin.runCommand({setFeatureCompatibilityVersion: "5.0"}),
                            ErrorCodes.CannotDowngrade));

        hangWaiter.failpoint.off();
        hangWaiter.waiter();
    }
}

{
    const rst = new ReplicaFixture();
    runTest(rst);
    rst.stop();
}

{
    const st = new ShardingFixture();
    runTest(st);
    st.stop();
}
}());
