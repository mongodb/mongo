/**
 * Loading this file extends the prototype for ReplSetTest to spawn a thread, which continuously
 * step down the primary.
 */

// Contains the declaration for ScopedThread and CountDownLatch
load('jstests/libs/parallelTester.js');
load("jstests/replsets/rslib.js");

/**
 * Executes the specified function and if it fails due to exception, which is related to network
 * error retries the call once. If the second attempt also fails, simply throws the last
 * exception.
 *
 * Returns the return value of the input call.
 */
function retryOnNetworkError(func) {
    var networkErrorRetriesLeft = 1;

    while (true) {
        try {
            return func();
        } catch (e) {
            if (e.toString().indexOf("network error") > -1 && networkErrorRetriesLeft > 0) {
                print("Network error occurred and the call will be retried: " +
                      tojson({error: e.toString(), stack: e.stack}));
                networkErrorRetriesLeft--;
            } else {
                throw e;
            }
        }
    }
}

(function() {
    'use strict';

    // Preserve the original ReplSetTest and ShardingTest constructors, because we are overriding
    // them
    var originalReplSetTest = ReplSetTest;
    var originalShardingTest = ShardingTest;

    const stepdownDelaySeconds = 10;
    const verbositySetting =
        "{ verbosity: 0, command: {verbosity: 1}, network: {verbosity: 1, asio: {verbosity: 2}}, \
tracking: {verbosity: 1} }";

    /**
     * Overrides the ReplSetTest constructor to start the continuous config server stepdown
     * thread.
     */
    ReplSetTest = function ReplSetTestWithContinuousPrimaryStepdown() {
        // Construct the original object
        originalReplSetTest.apply(this, arguments);

        /**
         * This function is intended to be called in a separate thread and it continuously steps
         * down the current primary for a number of attempts.
         *
         * @param {string} seedNode The connection string of a node from which to discover the
         *      primary of the replica set.
         * @param {CountDownLatch} stopCounter Object, which can be used to stop the thread.
         *
         * @param {integer} stepdownDelaySeconds The number of seconds after stepping down the
         *      primary for which the node is not re-electable.
         *
         * @return Object with the following fields:
         *      ok {integer}: 0 if it failed, 1 if it succeeded.
         *      error {string}: Only present if ok == 0. Contains the cause for the error.
         *      stack {string}: Only present if ok == 0. Contains the stack at the time of the
         *          error.
         */
        function _continuousPrimaryStepdownFn(seedNode, stopCounter, stepdownDelaySeconds) {
            'use strict';

            load('jstests/libs/override_methods/sharding_continuous_config_stepdown.js');

            print('*** Continuous stepdown thread running with seed node ' + seedNode);

            try {
                // The config primary may unexpectedly step down during startup if under heavy
                // load and too slowly processing heartbeats. When it steps down, it closes all of
                // its connections. This can happen during the call to new ReplSetTest, so in order
                // to account for this and make the tests stable, retry discovery of the replica
                // set's configuration once (SERVER-22794).
                var replSet = retryOnNetworkError(function() {
                    return new ReplSetTest(seedNode);
                });

                var primary = replSet.getPrimary();

                while (stopCounter.getCount() > 0) {
                    print('*** Stepping down ' + primary);

                    assert.throws(function() {
                        var result = primary.adminCommand(
                            {replSetStepDown: stepdownDelaySeconds, force: true});
                        print('replSetStepDown command did not throw and returned: ' +
                              tojson(result));

                        // The call to replSetStepDown should never succeed
                        assert.commandWorked(result);
                    });

                    // Wait for primary to get elected and allow the test to make some progress
                    // before attempting another stepdown.
                    if (stopCounter.getCount() > 0)
                        primary = replSet.getPrimary();

                    if (stopCounter.getCount() > 0)
                        sleep(8000);
                }

                print('*** Continuous stepdown thread completed successfully');
                return {ok: 1};
            } catch (e) {
                print('*** Continuous stepdown thread caught exception: ' + tojson(e));
                return {ok: 0, error: e.toString(), stack: e.stack};
            }
        }

        // Preserve the original stopSet method, because we are overriding it to stop the
        // continuous
        // stepdown thread.
        var _originalStartSetFn = this.startSet;
        var _originalStopSetFn = this.stopSet;

        // We override these methods to retry on network errors
        var _originalAwaitLastOpCommitted = this.awaitLastOpCommitted;

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
            if ('setParameter' in options) {
                options.setParameter.logComponentVerbosity = verbositySetting;
            } else {
                options.setParameter = {logComponentVerbosity: verbositySetting};
            }
            return _originalStartSetFn.call(this, options);
        };

        /**
         * Overrides the stopSet call so it terminates the failover thread.
         */
        this.stopSet = function() {
            this.stopContinuousFailover();
            _originalStopSetFn.apply(this, arguments);
        };

        /**
         * Overrides the awaitLastOpCommitted to retry on network errors.
         */
        this.awaitLastOpCommitted = function() {
            return retryOnNetworkError(_originalAwaitLastOpCommitted.bind(this));
        };

        /**
         * Spawns a thread to invoke continuousPrimaryStepdownFn. See its comments for more
         * information.
         */
        this.startContinuousFailover = function() {
            if (_scopedPrimaryStepdownThread) {
                throw new Error('Continuous failover thread is already active');
            }

            // This suite will step down the config primary every 10 seconds, and
            // electionTimeoutMillis defaults to 10 seconds. Set electionTimeoutMillis to 5 seconds,
            // so config operations have some time to run before being interrupted by stepdown.
            //
            // Note: this is done after ShardingTest runs because ShardingTest operations are not
            // resilient to stepdowns, which a shorter election timeout can cause to happen on
            // slow machines.
            var rsconfig = this.getReplSetConfigFromNode();
            rsconfig.settings.electionTimeoutMillis = stepdownDelaySeconds * 1000 / 2;
            rsconfig.version++;
            reconfig(this, rsconfig);
            assert.eq(this.getReplSetConfigFromNode().settings.electionTimeoutMillis,
                      5000,
                      "Failed to lower the electionTimeoutMillis to 5000 milliseconds.");

            _scopedPrimaryStepdownThreadStopCounter = new CountDownLatch(1);
            _scopedPrimaryStepdownThread = new ScopedThread(_continuousPrimaryStepdownFn,
                                                            this.nodes[0].host,
                                                            _scopedPrimaryStepdownThreadStopCounter,
                                                            stepdownDelaySeconds);
            _scopedPrimaryStepdownThread.start();
        };

        /**
         * Blocking method, which tells the thread running continuousPrimaryStepdownFn to stop
         * and
         * waits
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
        if ('configOptions' in arguments[0].other &&
            'setParameter' in arguments[0].other.configOptions) {
            arguments[0].other.configOptions.setParameter.logComponentVerbosity = verbositySetting;
        }

        if ('setParameter' in arguments[0].other) {
            arguments[0].other.setParameter.logComponentVerbosity = verbositySetting;
        } else {
            arguments[0].other.setParameter = {logComponentVerbosity: verbositySetting};
        }

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

        };

        // Start the continuous config server stepdown thread
        this.configRS.startContinuousFailover();
    };

    Object.extend(ShardingTest, originalShardingTest);

})();
