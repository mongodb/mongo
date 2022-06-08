// Test setClusterParameter command against FCV.
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

function mapToClusterParamsColl(db) {
    return db.getSiblingDB('config').clusterParameters;
}

function runTest(fixture) {
    // When the cluster is started at FCV 6.0, it is possible to run setClusterParameter.
    fixture.asAdmin(({admin}) => assert.commandWorked(admin.runCommand(
                        {setClusterParameter: {testIntClusterParameter: {intData: 102}}})));

    // Check that the config.clusterParameters collection has been created with a document for the
    // parameter.
    fixture.asAdmin(
        ({db}) => assert.eq(1, mapToClusterParamsColl(db).count({_id: "testIntClusterParameter"})));

    // When the cluster is at FCV 6.0 without an ongoing setClusterParameter operation in progress,
    // it should be possible to downgrade the cluster.
    fixture.asAdmin(({admin}) => assert.commandWorked(
                        admin.runCommand({setFeatureCompatibilityVersion: "5.0"})));

    // After downgrade, config.clusterParameters should not exist.
    fixture.asAdmin(({db}) => assert.isnull(mapToClusterParamsColl(db).exists()));
    fixture.asAdmin(
        ({db}) => assert.eq(
            0, mapToClusterParamsColl(db).count({_id: "testIntClusterParameter", intData: 102})));

    // While the cluster is downgraded, it should not be possible to run setClusterParameter.
    fixture.asAdmin(({admin}) => assert.commandFailed(admin.runCommand(
                        {setClusterParameter: {testIntClusterParameter: {intData: 102}}})));

    // Upgrading the cluster back to 6.0 should permit setClusterParameter to work again.
    fixture.asAdmin(({admin}) => assert.commandWorked(
                        admin.runCommand({setFeatureCompatibilityVersion: "6.0"})));
    fixture.asAdmin(({admin}) => assert.commandWorked(admin.runCommand(
                        {setClusterParameter: {testIntClusterParameter: {intData: 103}}})));

    // Set a failpoint to make setClusterParameter hang on a sharded cluster. FCV downgrade should
    // fail while setClusterParameter is in progress.
    if (fixture.hangTransition) {
        let hangWaiter =
            fixture.hangTransition({setClusterParameter: {testIntClusterParameter: {intData: 105}}},
                                   'hangInShardsvrSetClusterParameter');

        fixture.asAdmin(({admin}) => assert.commandFailedWithCode(
                            admin.runCommand({setFeatureCompatibilityVersion: "5.0"}),
                            ErrorCodes.CannotDowngrade));

        // Restart the config server primary and verify that FCV downgrade still fails.
        fixture.restartConfigPrimary();
        fixture.asAdmin(({admin}) => assert.commandFailedWithCode(
                            admin.runCommand({setFeatureCompatibilityVersion: "5.0"}),
                            ErrorCodes.CannotDowngrade));

        // Turn off the failpoint and wait for the hung setClusterParameter operation to drain.
        hangWaiter.failpoint.off();
        hangWaiter.waiter();

        // Verify that the updated value was successfully updated and is visible despite the restart
        // and failed FCV downgrade attempts.
        fixture.asAdmin(({admin}) => assert.eq(
                            105,
                            admin.runCommand({getClusterParameter: "testIntClusterParameter"})
                                .clusterParameters[0]
                                .intData));

        // Verify that FCV downgrade succeeds after the setClusterParameter operation has drained.
        fixture.asAdmin(({admin}) => assert.commandWorked(
                            admin.runCommand({setFeatureCompatibilityVersion: "5.0"})));
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
