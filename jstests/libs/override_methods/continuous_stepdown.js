/**
 * Loading this file exposes ContinuousStepdown, which contains the "configure" function that
 * extends the prototype for ReplSetTest to spawn a thread that continuously step down its primary
 * node.
 *
 * ContinuousStepdown#configure takes a configuration object with the following options:
 *
 *    configStepdown: boolean (default true)
 *        True if a stepdown thread should be started for the CSRS.
 *
 *    electionTimeoutMS: number (default 5 seconds)
 *        The election timeout for the replica set.
 *
 *    shardStepdown: boolean (default true)
 *        True if a stepdown thread should be started for each shard replica set.
 *
 *    stepdownDurationSecs: number (default 10 seconds)
 *        Number of seconds after stepping down as primary for which the node is not re-electable.
 *
 *    stepdownIntervalMS: number (default 8 seconds)
 *        Number of milliseconds to wait after issuing a step down command, and discovering the new
 *        primary.
 *
 *    catchUpTimeoutMS: number (default 0 seconds)
 *        The amount of time allowed for newly-elected primaries to catch up.
 */

let ContinuousStepdown;

(function() {
    "use strict";

    load("jstests/libs/parallelTester.js");  // ScopedThread and CountDownLatch
    load("jstests/replsets/rslib.js");       // reconfig

    /**
     * Helper class to manage the ScopedThread instance that will continuously step down the primary
     * node.
     */
    const StepdownThread = function() {
        let _counter = null;
        let _thread = null;

        /**
         * This function is intended to be called in a separate thread and it continuously
         * steps down the current primary for a number of attempts.
         *
         * @param {CountDownLatch} stopCounter Object, which can be used to stop the thread.
         *
         * @param {string} seedNode The connection string of a node from which to discover
         *      the primary of the replica set.
         *
         * @param {Object} options Configuration object with the following fields:
         *      stepdownDurationSecs {integer}: The number of seconds after stepping down the
         *          primary for which the node is not re-electable.
         *      stepdownIntervalMS {integer}: The number of milliseconds to wait after
         *          issuing a step down command.
         *
         * @return Object with the following fields:
         *      ok {integer}: 0 if it failed, 1 if it succeeded.
         *      error {string}: Only present if ok == 0. Contains the cause for the error.
         *      stack {string}: Only present if ok == 0. Contains the stack at the time of
         *          the error.
         */
        function _continuousPrimaryStepdownFn(stopCounter, seedNode, options) {
            "use strict";

            print("*** Continuous stepdown thread running with seed node " + seedNode);

            try {
                // The config primary may unexpectedly step down during startup if under heavy
                // load and too slowly processing heartbeats. When it steps down, it closes all of
                // its connections. ReplSetTest will therefore retry discovery of the replica set's
                // config.
                const replSet = new ReplSetTest(seedNode);

                let primary = replSet.getPrimary();

                while (stopCounter.getCount() > 0) {
                    print("*** Stepping down " + primary);

                    assert.throws(function() {
                        let result = primary.adminCommand(
                            {replSetStepDown: options.stepdownDurationSecs, force: true});
                        print("replSetStepDown command did not throw and returned: " +
                              tojson(result));

                        // The call to replSetStepDown should never succeed.
                        assert.commandWorked(result);
                    });

                    // Wait for primary to get elected and allow the test to make some progress
                    // before attempting another stepdown.
                    if (stopCounter.getCount() > 0) {
                        primary = replSet.getPrimary();
                    }

                    if (stopCounter.getCount() > 0) {
                        sleep(options.stepdownIntervalMS);
                    }
                }

                print("*** Continuous stepdown thread completed successfully");
                return {ok: 1};
            } catch (e) {
                print("*** Continuous stepdown thread caught exception: " + tojson(e));
                return {ok: 0, error: e.toString(), stack: e.stack};
            }
        }

        /**
         * Returns true if the stepdown thread has been created and started.
         */
        this.hasStarted = function() {
            return !!_thread;
        };

        /**
         * Spawns a ScopedThread using the given seedNode to discover the replica set.
         */
        this.start = function(seedNode, options) {
            if (_thread) {
                throw new Error("Continuous stepdown thread is already active");
            }

            _counter = new CountDownLatch(1);
            _thread = new ScopedThread(_continuousPrimaryStepdownFn, _counter, seedNode, options);
            _thread.start();
        };

        /**
         * Sets the stepdown thread's counter to 0, and waits for it to finish. Throws if the
         * stepdown thread did not exit successfully.
         */
        this.stop = function() {
            if (!_thread) {
                throw new Error("Continuous stepdown thread is not active");
            }

            _counter.countDown();
            _counter = null;

            _thread.join();

            const retVal = _thread.returnData();
            _thread = null;

            assert.commandWorked(retVal);
        };
    };

    ContinuousStepdown = {};

    /**
     * Defines two methods on ReplSetTest, startContinuousFailover and stopContinuousFailover, that
     * allow starting and stopping a separate thread that will periodically step down the replica
     * set's primary node. Also defines these methods on ShardingTest, which allow starting and
     * stopping a stepdown thread for the test's config server replica set and each of the shard
     * replica sets, as specified by the given stepdownOptions object.
     */
    ContinuousStepdown.configure = function(stepdownOptions,
                                            {verbositySetting: verbositySetting = {}} = {}) {
        const defaultOptions = {
            configStepdown: true,
            electionTimeoutMS: 5 * 1000,
            shardStepdown: true,
            stepdownDurationSecs: 10,
            stepdownIntervalMS: 8 * 1000,
            catchUpTimeoutMS: 0,
        };
        stepdownOptions = Object.merge(defaultOptions, stepdownOptions);

        verbositySetting = tojson(verbositySetting);

        // Preserve the original ReplSetTest and ShardingTest constructors, because they are being
        // overriden.
        const originalReplSetTest = ReplSetTest;
        const originalShardingTest = ShardingTest;

        /**
         * Overrides the ReplSetTest constructor to start the continuous primary stepdown thread.
         */
        ReplSetTest = function ReplSetTestWithContinuousPrimaryStepdown() {
            // Construct the original object
            originalReplSetTest.apply(this, arguments);

            // Preserve the original versions of functions that are overrided below.
            const _originalStartSetFn = this.startSet;
            const _originalStopSetFn = this.stopSet;
            const _originalAwaitLastOpCommitted = this.awaitLastOpCommitted;

            /**
             * Overrides startSet call to increase logging verbosity.
             */
            this.startSet = function() {
                let options = arguments[0] || {};

                if (typeof(options.setParameter) === "string") {
                    var eqIdx = options.setParameter.indexOf("=");
                    if (eqIdx != -1) {
                        var param = options.setParameter.substring(0, eqIdx);
                        var value = options.setParameter.substring(eqIdx + 1);
                        options.setParameter = {};
                        options.setParameter[param] = value;
                    }
                }
                arguments[0] = options;

                options.setParameter = options.setParameter || {};
                options.setParameter.logComponentVerbosity = verbositySetting;
                return _originalStartSetFn.apply(this, arguments);
            };

            /**
             * Overrides stopSet to terminate the failover thread.
             */
            this.stopSet = function() {
                this.stopContinuousFailover({waitForPrimary: false});
                _originalStopSetFn.apply(this, arguments);
            };

            /**
             * Overrides awaitLastOpCommitted to retry on network errors.
             */
            this.awaitLastOpCommitted = function() {
                return retryOnNetworkError(_originalAwaitLastOpCommitted.bind(this));
            };

            // Handle for the continuous stepdown thread.
            const _stepdownThread = new StepdownThread();

            /**
             * Reconfigures the replica set, then starts the stepdown thread. As part of the new
             * config, this sets:
             * - electionTimeoutMillis to stepdownOptions.electionTimeoutMS so a new primary can
             *   get elected before the stepdownOptions.stepdownIntervalMS period would cause one
             *   to step down again.
             * - catchUpTimeoutMillis to stepdownOptions.catchUpTimeoutMS. Lower values increase
             *   the likelihood and volume of rollbacks.
             */
            this.startContinuousFailover = function() {
                if (_stepdownThread.hasStarted()) {
                    throw new Error("Continuous failover thread is already active");
                }

                const rsconfig = this.getReplSetConfigFromNode();

                const shouldUpdateElectionTimeout =
                    (rsconfig.settings.electionTimeoutMillis !== stepdownOptions.electionTimeoutMS);
                const shouldUpdateCatchUpTimeout =
                    (rsconfig.settings.catchUpTimeoutMillis !== stepdownOptions.catchUpTimeoutMS);

                if (shouldUpdateElectionTimeout || shouldUpdateCatchUpTimeout) {
                    rsconfig.settings.electionTimeoutMillis = stepdownOptions.electionTimeoutMS;
                    rsconfig.settings.catchUpTimeoutMillis = stepdownOptions.catchUpTimeoutMS;

                    rsconfig.version += 1;
                    reconfig(this, rsconfig);

                    const newSettings = this.getReplSetConfigFromNode().settings;

                    assert.eq(newSettings.electionTimeoutMillis,
                              stepdownOptions.electionTimeoutMS,
                              "Failed to set the electionTimeoutMillis to " +
                                  stepdownOptions.electionTimeoutMS + " milliseconds.");
                    assert.eq(newSettings.catchUpTimeoutMillis,
                              stepdownOptions.catchUpTimeoutMS,
                              "Failed to set the catchUpTimeoutMillis to " +
                                  stepdownOptions.catchUpTimeoutMS + " milliseconds.");
                }

                _stepdownThread.start(this.nodes[0].host, stepdownOptions);
            };

            /**
             * Blocking method, which tells the thread running continuousPrimaryStepdownFn to stop
             * and waits for it to terminate.
             *
             * If waitForPrimary is true, blocks until a new primary has been elected.
             */
            this.stopContinuousFailover = function({waitForPrimary: waitForPrimary = false} = {}) {
                if (!_stepdownThread.hasStarted()) {
                    return;
                }

                _stepdownThread.stop();

                if (waitForPrimary) {
                    this.getPrimary();
                }
            };
        };

        Object.extend(ReplSetTest, originalReplSetTest);

        /**
         * Overrides the ShardingTest constructor to start the continuous primary stepdown thread.
         */
        ShardingTest = function ShardingTestWithContinuousPrimaryStepdown(params) {
            params.other = params.other || {};

            if (stepdownOptions.configStepdown) {
                params.other.configOptions = params.other.configOptions || {};
                params.other.configOptions.setParameter =
                    params.other.configOptions.setParameter || {};
                params.other.configOptions.setParameter.logComponentVerbosity = verbositySetting;
            }

            if (stepdownOptions.shardStepdown) {
                params.other.shardOptions = params.other.shardOptions || {};
                params.other.shardOptions.setParameter =
                    params.other.shardOptions.setParameter || {};
                params.other.shardOptions.setParameter.logComponentVerbosity = verbositySetting;
            }

            // Construct the original object.
            originalShardingTest.apply(this, arguments);

            // Validate the stepdown options.
            if (stepdownOptions.configStepdown && !this.configRS) {
                throw new Error(
                    "Continuous config server primary step down only available with CSRS");
            }

            if (stepdownOptions.shardStepdown && this._rs.some(rst => !rst)) {
                throw new Error(
                    "Continuous shard primary step down only available with replica set shards");
            }

            /**
             * Calls startContinuousFailover on the config server and/or each shard replica set as
             * specifed by the stepdownOptions object.
             */
            this.startContinuousFailover = function() {
                if (stepdownOptions.configStepdown) {
                    this.configRS.startContinuousFailover();
                }

                if (stepdownOptions.shardStepdown) {
                    this._rs.forEach(function(rst) {
                        rst.test.startContinuousFailover();
                    });
                }
            };

            /**
             * Calls stopContinuousFailover on the config server and each shard replica set as
             * specified by the stepdownOptions object.
             *
             * If waitForPrimary is true, blocks until each replica set has elected a primary.
             * If waitForMongosRetarget is true, blocks until each mongos has an up to date view of
             * the cluster.
             */
            this.stopContinuousFailover = function({
                waitForPrimary: waitForPrimary = false,
                waitForMongosRetarget: waitForMongosRetarget = false
            } = {}) {
                if (stepdownOptions.configStepdown) {
                    this.configRS.stopContinuousFailover({waitForPrimary: waitForPrimary});
                }

                if (stepdownOptions.shardStepdown) {
                    this._rs.forEach(function(rst) {
                        rst.test.stopContinuousFailover({waitForPrimary: waitForPrimary});
                    });
                }

                if (waitForMongosRetarget) {
                    // Run validate on each collection in each database to ensure mongos can target
                    // the primary for each shard with data, including the config servers.
                    this._mongos.forEach(s => {
                        const res = assert.commandWorked(s.adminCommand({listDatabases: 1}));
                        res.databases.forEach(dbInfo => {
                            const startTime = Date.now();
                            print("Waiting for mongos: " + s.host + " to retarget db: " +
                                  dbInfo.name);

                            const db = s.getDB(dbInfo.name);
                            assert.soon(() => {
                                let collInfo;
                                try {
                                    collInfo = db.getCollectionInfos();
                                } catch (e) {
                                    if (ErrorCodes.isNotMasterError(e.code)) {
                                        return false;
                                    }
                                    throw e;
                                }

                                collInfo.forEach(collDoc => {
                                    const res = db.runCommand({collStats: collDoc["name"]});
                                    if (ErrorCodes.isNotMasterError(res.code)) {
                                        return false;
                                    }
                                    assert.commandWorked(res);
                                });

                                return true;
                            });
                            const totalTime = Date.now() - startTime;
                            print("Finished waiting for mongos: " + s.host + " to retarget db: " +
                                  dbInfo.name + ", in " + totalTime + " ms");
                        });
                    });
                }

            };

            /**
             * This method is disabled because it runs aggregation, which doesn't handle config
             * server stepdown correctly.
             */
            this.printShardingStatus = function() {};
        };

        Object.extend(ShardingTest, originalShardingTest);

        // The checkUUIDsConsistentAcrossCluster() function is defined on ShardingTest's prototype,
        // but ShardingTest's prototype gets reset when ShardingTest is reassigned. We reload the
        // override to redefine checkUUIDsConsistentAcrossCluster() on the new ShardingTest's
        // prototype.
        load('jstests/libs/override_methods/check_uuids_consistent_across_cluster.js');
    };
})();
