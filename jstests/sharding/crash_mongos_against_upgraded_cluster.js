// A mongos participating in a cluster that has been upgraded both a binary and FCV version above it
// should crash.
//
// This kind of scenario can happen when a user forgets to upgrade the mongos binary and then calls
// setFCV(upgrade), leaving the still downgraded mongos unable to communicate. Rather than the
// mongos logging incompatible server version errors endlessly, we've chosen to crash it.
//
// We use the 'impersonateFullyUpgradedFutureVersion' fail point to make a mongod return min and max
// wire version that simulate a fully upgraded mongod with which the mongos should not communicate.

TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
    "use strict";

    var st = new ShardingTest({mongos: 1, shards: 1});
    var ns = "testDB.testColl";
    var mongos;

    // There are two network code paths on the mongos: the ShardingTaskExecutor and the
    // DBClientConnection. We must test both. There is code in both that crash a mongos on receiving
    // an IncompatibleWithUpgradedServer error from lower in the network layer.

    // Turn off the DBClientConnection IncompatibleWithUpgradedServer check so that we know we're
    // crashing because of the ShardingTaskExecutor check.
    //
    // Then set impersonateFullyUpgradedFutureVersion on the shard server and insert a document
    // through the mongos: the mongos write command uses a ShardingTaskExecutor.

    jsTest.log('Setting failpoint turnOffDBClientIncompatibleWithUpgradedServerCheck on mongos');
    assert.commandWorked(st.s0.adminCommand({
        configureFailPoint: 'turnOffDBClientIncompatibleWithUpgradedServerCheck',
        mode: 'alwaysOn'
    }));

    jsTest.log('Setting failpoint impersonateFullyUpgradedFutureVersion on shard');
    assert.commandWorked(st.shard0.adminCommand(
        {configureFailPoint: 'impersonateFullyUpgradedFutureVersion', mode: 'alwaysOn'}));

    jsTest.log('Sending an insert through the mongos to provoke a crash');

    var error = assert.throws(function() {
        st.s0.getDB("testDB").runCommand({insert: "testColl", documents: [{a: 1}]});
    });
    assert(isNetworkError(error));

    // Check that the server shut down.
    st.stopMongos(0, {allowedExitCode: MongoRunner.EXIT_ABRUPT});

    // No need to turn off the mongos failpoint, since it has crashed.

    jsTest.log('Turning off failpoint impersonateFullyUpgradedFutureVersion on shard');
    assert.commandWorked(st.shard0.adminCommand(
        {configureFailPoint: 'impersonateFullyUpgradedFutureVersion', mode: 'off'}));

    // We can't test DBClientConnection directly through a mongos command. However, starting up a
    // mongos currently uses a DBClientConnection with the config server, so we rely on that to test
    // it.

    jsTest.log('Setting failpoint impersonateFullyUpgradedFutureVersion on the config server.');
    assert.commandWorked(st.config0.adminCommand(
        {configureFailPoint: 'impersonateFullyUpgradedFutureVersion', mode: 'alwaysOn'}));

    jsTest.log(
        'Starting up a new mongos, which should crash because the config failpoint is live.');

    mongos = MongoRunner.runMongos({configdb: st.configRS.getURL()});
    assert(!mongos);

    jsTest.log('Turning off failpoint impersonateFullyUpgradedFutureVersion on the config server');
    assert.commandWorked(st.config0.adminCommand(
        {configureFailPoint: 'impersonateFullyUpgradedFutureVersion', mode: 'off'}));

    // Ensure we can start up a mongos with regular state.

    jsTest.log(
        'Starting up a new mongos, which should succeed because all the fail points have been turned off.');
    mongos = MongoRunner.runMongos({configdb: st.configRS.getURL()});
    assert(mongos);

    st.stop();
    MongoRunner.stopMongos(mongos);

})();
