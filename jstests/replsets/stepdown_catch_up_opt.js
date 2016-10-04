/*
 * Test to ensure the secondaryCatchUpPeriodSecs option in the replSetStepDown command is parsed
 * correctly, and will affect the delay appropriately.
 */

(function() {
    'use strict';
    var name = 'stepdown_catch_up_opt';
    // Only 2 nodes, so that we can control whether the secondary is caught up.
    var replTest = new ReplSetTest({name: name, nodes: 2});
    replTest.startSet();
    replTest.initiate();
    replTest.awaitSecondaryNodes();
    var primary = replTest.getPrimary();
    var secondary = replTest.getSecondary();

    // Error codes we expect to see.

    // If the secondary is not caught up.
    var noCaughtUpSecondariesCode = 50;

    // If the stepdown period is shorter than the secondaryCatchUpPeriodSecs argument.
    var stepDownPeriodTooShortCode = 2;

    // If we give a string as an argument instead of an integer.
    var stringNotIntCode = 14;

    // Expect a failure with a string argument.
    assert.commandFailedWithCode(
        primary.getDB('admin').runCommand({replSetStepDown: 10, secondaryCatchUpPeriodSecs: 'STR'}),
        stringNotIntCode,
        'Expected string argument to secondaryCatchupPeriodSecs to fail.');

    // Expect a failure with a longer secondaryCatchupPeriodSecs than the stepdown period.
    assert.commandFailedWithCode(
        primary.getDB('admin').runCommand({replSetStepDown: 10, secondaryCatchUpPeriodSecs: 20}),
        stepDownPeriodTooShortCode,
        ('Expected replSetStepDown to fail given a stepdown time shorter than' +
         ' secondaryCatchUpPeriodSecs'));

    jsTestLog('Stop secondary syncing.');
    assert.commandWorked(secondary.getDB('admin').runCommand(
                             {configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'}),
                         'Failed to configure rsSyncApplyStop failpoint.');

    function disableFailPoint() {
        assert.commandWorked(secondary.getDB('admin').runCommand(
                                 {configureFailPoint: 'rsSyncApplyStop', mode: 'off'}),
                             'Failed to disable rsSyncApplyStop failpoint.');
    }

    // If any of these assertions fail, we need to disable the fail point in order for the mongod to
    // shut down.
    try {
        jsTestLog('Write to primary to make secondary out of sync.');
        assert.writeOK(primary.getDB('test').foo.insert({i: 1}), 'Failed to insert document.');
        sleep(1000);
        // Secondary is now at least 1 second behind.

        jsTestLog('Try to step down.');
        var startTime = new Date();
        assert.commandFailedWithCode(
            primary.getDB('admin').runCommand({replSetStepDown: 10, secondaryCatchUpPeriodSecs: 1}),
            noCaughtUpSecondariesCode,
            'Expected replSetStepDown to fail, since no secondaries should be caught up.');
        var endTime = new Date();

        // Ensure it took at least 1 second to time out. Adjust the timeout a little bit
        // for the precision issue of clock on Windows 2K8.
        assert.lte(0.95,
                   (endTime - startTime) / 1000,
                   'Expected replSetStepDown command to fail after 1 second.');
    } catch (err) {
        disableFailPoint();
        throw err;
    }

    disableFailPoint();

    // Make sure the primary hasn't changed, since all stepdowns should have failed.
    var primaryStatus = primary.getDB('admin').runCommand({replSetGetStatus: 1});
    assert.commandWorked(primaryStatus, 'replSetGetStatus failed.');
    assert.eq(primaryStatus.myState,
              ReplSetTest.State.PRIMARY,
              'Expected original primary node to still be primary');

    replTest.stopSet();
}());
