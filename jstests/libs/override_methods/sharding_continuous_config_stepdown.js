/**
 * Loading this file extends the prototype for ReplSetTest to spawn a thread, which continuously
 * step down the primary.
 */

// Contains the declaration for ScopedThread and CountDownLatch
load('jstests/libs/parallelTester.js');

(function() {
'use strict';

// Preserve the original ReplSetTest and ShardingTest constructors, because we are overriding them
var originalReplSetTest = ReplSetTest;
var originalShardingTest = ShardingTest;

/**
 * Overrides the ReplSetTest constructor to start the continuous config server stepdown thread.
 */
ReplSetTest = function ReplSetTestWithContinuousPrimaryStepdown() {
    // Construct the original object
    originalReplSetTest.apply(this, arguments);

    /**
     * This function is intended to be called in a separate thread and it continuously steps down
     * the current primary for a number of attempts.
     *
     * @param {string} seedNode The connection string of a node from which to discover the primary
     *                          of the replica set.
     * @param {CountDownLatch} stopCounter Object, which can be used to stop the thread.
     *
     * @return Object with the following fields:
     *      ok {integer}: 0 if it failed, 1 if it succeeded.
     *      error {string}: Only present if ok == 0. Contains the cause for the error.
     *      stack {string}: Only present if ok == 0. Contains the stack at the time of the error.
     */
    function _continuousPrimaryStepdownFn(seedNode, stopCounter) {
        'use strict';

        var stepdownDelaySeconds = 10;

        print('*** Continuous stepdown thread running with seed node ' + seedNode);

        try {
            var replSet = new ReplSetTest(seedNode);
            var primary = replSet.getPrimary();

            while (stopCounter.getCount() > 0) {
                print('*** Stepping down ' + primary);

                assert.throws(function() {
                    var result = primary.adminCommand({
                        replSetStepDown: stepdownDelaySeconds,
                        secondaryCatchUpPeriodSecs: stepdownDelaySeconds });
                    print('replSetStepDown command did not throw and returned: ' + tojson(result));

                    // The call to replSetStepDown should never succeed
                    assert.commandWorked(result);
                });

                // Wait for primary to get elected and allow the test to make some progress before
                // attempting another stepdown.
                if (stopCounter.getCount() > 0)
                    primary = replSet.getPrimary();

                if (stopCounter.getCount() > 0)
                    sleep(8000);
            }

            print('*** Continuous stepdown thread completed successfully');
            return { ok: 1 };
        }
        catch (e) {
            print('*** Continuous stepdown thread caught exception: ' + tojson(e));
            return { ok: 0, error: e.toString(), stack: e.stack };
        }
    }

    // Preserve the original stopSet method, because we are overriding it to stop the continuous
    // stepdown thread.
    var _originalStartSetFn = this.startSet;
    var _originalStopSetFn = this.stopSet;

    // These two manage the scoped failover thread
    var _scopedPrimaryStepdownThread;
    var _scopedPrimaryStepdownThreadStopCounter;

    /**
     * Overrides the startSet call so we can increase the logging verbosity
     */
    this.startSet = function(options) {
        if (!options) {
            options = {};
        }
        options.verbose = 2;
        return _originalStartSetFn.call(this, options);
    }

    /**
     * Overrides the stopSet call so it terminates the failover thread.
     */
    this.stopSet = function() {
        this.stopContinuousFailover();
        _originalStopSetFn.apply(this, arguments);
    };

    /**
     * Spawns a thread to invoke continuousPrimaryStepdownFn. See its comments for more information.
     */
    this.startContinuousFailover = function() {
        if (_scopedPrimaryStepdownThread) {
            throw new Error('Continuous failover thread is already active');
        }

        _scopedPrimaryStepdownThreadStopCounter = new CountDownLatch(1);
        _scopedPrimaryStepdownThread = new ScopedThread(_continuousPrimaryStepdownFn,
                                                        this.nodes[0].host,
                                                        _scopedPrimaryStepdownThreadStopCounter);
        _scopedPrimaryStepdownThread.start();
    };

    /**
     * Blocking method, which tells the thread running continuousPrimaryStepdownFn to stop and waits
     * for it to terminate.
     */
    this.stopContinuousFailover = function() {
        if (!_scopedPrimaryStepdownThread) {
            return;
        }

        _scopedPrimaryStepdownThreadStopCounter.countDown();
        _scopedPrimaryStepdownThreadStopCounter = null;

        _scopedPrimaryStepdownThread.join();

        var retVal = _scopedPrimaryStepdownThread.returnData();
        _scopedPrimaryStepdownThread = null;

        return assert.commandWorked(retVal);
    };
};

Object.extend(ReplSetTest, originalReplSetTest);

/**
 * Overrides the ShardingTest constructor to start the continuous config server stepdown thread.
 */
ShardingTest = function ShardingTestWithContinuousConfigPrimaryStepdown() {
    if (!arguments[0].other) {
        arguments[0].other = {};
    }
    arguments[0].verbose = 2;

    if (!arguments[0].other.shardOptions) {
        arguments[0].other.shardOptions = {};
    }
    arguments[0].other.shardOptions.verbose = 2;

    // Construct the original object
    originalShardingTest.apply(this, arguments);

    if (!this.configRS) {
        throw new Error('Continuous config server step down only available with CSRS');
    }

    /**
     * This method is disabled because it runs aggregation, which doesn't handle config server
     * stepdown correctly.
     */
    this.printShardingStatus = function() {

    }

    // Start the continuous config server stepdown thread
    this.configRS.startContinuousFailover();
};

Object.extend(ShardingTest, originalShardingTest);

})();
