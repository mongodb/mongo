/**
 * This tests the behavior of changing the MemberConfig field 'secondaryDelaySecs' to 'slaveDelay'
 * on downgrade and vice versa on upgrade.
 */

(function() {
"use strict";

function runTest(downgradeFCV) {
    const replTest = new ReplSetTest(
        {name: 'testSet', nodes: [{}, {rsConfig: {priority: 0, secondaryDelaySecs: 10}}]});
    replTest.startSet();
    replTest.initiate();

    const primary = replTest.getPrimary();

    // Test that the current config has the expected fields.
    let config = primary.adminCommand({replSetGetConfig: 1}).config;

    assert.eq(config.members[1].secondaryDelaySecs, 10, tojson(config));
    assert.eq(config.members[1].hasOwnProperty('slaveDelay'), false, tojson(config));

    // Test that a reconfig with 'secondaryDelaySecs' works.
    config.version++;
    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

    delete config.members[1].secondaryDelaySecs;
    config.members[1].slaveDelay = 10;
    config.version++;

    // Test that a reconfig with 'slaveDelay' fails.
    assert.commandFailedWithCode(primary.adminCommand({replSetReconfig: config}),
                                 ErrorCodes.NewReplicaSetConfigurationIncompatible);

    // We set FCV to the downgraded FCV. After downgrading, ensure the new config has the
    // correct fields.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: downgradeFCV}));

    config = primary.adminCommand({replSetGetConfig: 1}).config;
    assert.eq(config.members[1].slaveDelay, 10, tojson(config));
    assert.eq(config.members[1].hasOwnProperty('secondaryDelaySecs'), false, tojson(config));

    config.version++;
    // Test that a reconfig with 'slaveDelay' works.
    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

    delete config.members[1].slaveDelay;
    config.members[1].secondaryDelaySecs = 10;
    config.version++;

    // Test that a reconfig with 'secondaryDelaySecs' fails.
    assert.commandFailedWithCode(primary.adminCommand({replSetReconfig: config}),
                                 ErrorCodes.NewReplicaSetConfigurationIncompatible);

    // We set FCV back to latest FCV. After upgrading, ensure the new config has the
    // correct fields.
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    config = primary.adminCommand({replSetGetConfig: 1}).config;
    assert.eq(config.members[1].secondaryDelaySecs, 10, tojson(config));
    assert.eq(config.members[1].hasOwnProperty('slaveDelay'), false, tojson(config));

    config.version++;
    // Test that a reconfig with 'secondaryDelaySecs' works.
    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

    delete config.members[1].secondaryDelaySecs;
    config.members[1].slaveDelay = 10;
    config.version++;

    // Test that a reconfig with 'slaveDelay' fails.
    assert.commandFailedWithCode(primary.adminCommand({replSetReconfig: config}),
                                 ErrorCodes.NewReplicaSetConfigurationIncompatible);

    replTest.stopSet();
}

runFeatureFlagMultiversionTest('featureFlagUseSecondaryDelaySecs', runTest);
}());