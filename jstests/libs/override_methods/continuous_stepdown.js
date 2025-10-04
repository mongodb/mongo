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

import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {reconfig, reconnect} from "jstests/replsets/rslib.js";

export class ContinuousStepdown {
    /**
     * Defines two methods on ReplSetTest, startContinuousFailover and stopContinuousFailover, that
     * allow starting and stopping a separate thread that will periodically step down the replica
     * set's primary node. Also defines these methods on ShardingTest, which allow starting and
     * stopping a stepdown thread for the test's config server replica set and each of the shard
     * replica sets, as specified by the given stepdownOptions object.
     */
    static configure(stepdownOptions, {verbositySetting: verbositySetting = {}} = {}) {
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

        return {
            ReplSetTestWithContinuousPrimaryStepdown: makeReplSetTestWithContinuousPrimaryStepdown(
                stepdownOptions,
                verbositySetting,
            ),
            ShardingTestWithContinuousPrimaryStepdown: makeShardingTestWithContinuousPrimaryStepdown(
                stepdownOptions,
                verbositySetting,
            ),
        };
    }
}

/**
 * Helper class to manage the Thread instance that will continuously step down the primary
 * node.
 */
const StepdownThread = function () {
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
        jsTest.log.info("*** Continuous stepdown thread running with seed node " + seedNode);

        try {
            // The config primary may unexpectedly step down during startup if under heavy
            // load and too slowly processing heartbeats.
            const replSet = new ReplSetTest(seedNode);

            let primary = replSet.getPrimary();

            while (stopCounter.getCount() > 0) {
                jsTest.log.info("*** Stepping down " + primary);

                // The command may fail if the node is no longer primary or is in the process of
                // stepping down.
                assert.commandWorkedOrFailedWithCode(
                    primary.adminCommand({replSetStepDown: options.stepdownDurationSecs, force: true}),
                    [ErrorCodes.NotWritablePrimary, ErrorCodes.ConflictingOperationInProgress],
                );

                // Wait for primary to get elected and allow the test to make some progress
                // before attempting another stepdown.
                if (stopCounter.getCount() > 0) {
                    primary = replSet.getPrimary();
                }

                if (stopCounter.getCount() > 0) {
                    sleep(options.stepdownIntervalMS);
                }
            }

            jsTest.log.info("*** Continuous stepdown thread completed successfully");
            return {ok: 1};
        } catch (e) {
            jsTest.log.info("*** Continuous stepdown thread caught exception", {error: e});
            return {ok: 0, error: e.toString(), stack: e.stack};
        }
    }

    /**
     * Returns true if the stepdown thread has been created and started.
     */
    this.hasStarted = function () {
        return !!_thread;
    };

    /**
     * Spawns a Thread using the given seedNode to discover the replica set.
     */
    this.start = function (seedNode, options) {
        if (_thread) {
            throw new Error("Continuous stepdown thread is already active");
        }

        _counter = new CountDownLatch(1);
        _thread = new Thread(_continuousPrimaryStepdownFn, _counter, seedNode, options);
        _thread.start();
    };

    /**
     * Sets the stepdown thread's counter to 0, and waits for it to finish. Throws if the
     * stepdown thread did not exit successfully.
     */
    this.stop = function () {
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

/**
 * Overrides the ReplSetTest constructor to start the continuous primary stepdown thread.
 */
function makeReplSetTestWithContinuousPrimaryStepdown(stepdownOptions, verbositySetting) {
    return class ReplSetTestWithContinuousPrimaryStepdown extends ReplSetTest {
        constructor(options) {
            super(options);
            // Handle for the continuous stepdown thread.
            this._stepdownThread = new StepdownThread();
            // Preserve the original set of nodeOptions passed to the constructor.
            this._origNodeOpts = Object.assign({}, (options && options.nodeOptions) || {});
        }

        /**
         * Overrides startSet call to increase logging verbosity. Ensure that we only override the
         * 'logComponentVerbosity' server parameter, but retain any other parameters that were
         * supplied during ReplSetTest construction.
         */
        startSet(options, restart) {
            // Helper function to convert a string representation of setParameter to object form.
            function setParamToObj(setParam) {
                if (typeof setParam === "string") {
                    let eqIdx = setParam.indexOf("=");
                    if (eqIdx != -1) {
                        let param = setParam.substring(0, eqIdx);
                        let value = setParam.substring(eqIdx + 1);
                        return {[param]: value};
                    }
                }
                return Object.assign({}, setParam || {});
            }

            options = options || {};
            options.setParameter = Object.assign(
                setParamToObj(this._origNodeOpts.setParameter),
                setParamToObj(options.setParameter),
                {logComponentVerbosity: verbositySetting},
            );
            return super.startSet(options, restart);
        }

        /**
         * Overrides stopSet to terminate the failover thread.
         */
        stopSet(signal, forRestart, options) {
            this.stopContinuousFailover({waitForPrimary: true});
            super.stopSet(signal, forRestart, options);
        }

        /**
         * Overrides awaitLastOpCommitted to retry on network errors.
         */
        awaitLastOpCommitted() {
            return retryOnNetworkError(() => super.awaitLastOpCommitted());
        }

        /**
         * Reconfigures the replica set, then starts the stepdown thread. As part of the new
         * config, this sets:
         * - electionTimeoutMillis to stepdownOptions.electionTimeoutMS so a new primary can
         *   get elected before the stepdownOptions.stepdownIntervalMS period would cause one
         *   to step down again.
         * - catchUpTimeoutMillis to stepdownOptions.catchUpTimeoutMS. Lower values increase
         *   the likelihood and volume of rollbacks.
         */
        startContinuousFailover() {
            if (this._stepdownThread.hasStarted()) {
                throw new Error("Continuous failover thread is already active");
            }

            const rsconfig = this.getReplSetConfigFromNode();

            const shouldUpdateElectionTimeout =
                rsconfig.settings.electionTimeoutMillis !== stepdownOptions.electionTimeoutMS;
            const shouldUpdateCatchUpTimeout =
                rsconfig.settings.catchUpTimeoutMillis !== stepdownOptions.catchUpTimeoutMS;

            if (shouldUpdateElectionTimeout || shouldUpdateCatchUpTimeout) {
                rsconfig.settings.electionTimeoutMillis = stepdownOptions.electionTimeoutMS;
                rsconfig.settings.catchUpTimeoutMillis = stepdownOptions.catchUpTimeoutMS;

                rsconfig.version += 1;
                reconfig(this, rsconfig);

                const newSettings = this.getReplSetConfigFromNode().settings;

                assert.eq(
                    newSettings.electionTimeoutMillis,
                    stepdownOptions.electionTimeoutMS,
                    "Failed to set the electionTimeoutMillis to " +
                        stepdownOptions.electionTimeoutMS +
                        " milliseconds.",
                );
                assert.eq(
                    newSettings.catchUpTimeoutMillis,
                    stepdownOptions.catchUpTimeoutMS,
                    "Failed to set the catchUpTimeoutMillis to " + stepdownOptions.catchUpTimeoutMS + " milliseconds.",
                );
            }

            this._stepdownThread.start(this.nodes[0].host, stepdownOptions);
        }

        /**
         * Blocking method, which tells the thread running continuousPrimaryStepdownFn to stop
         * and waits for it to terminate.
         *
         * If waitForPrimary is true, blocks until a new primary has been elected and reestablishes
         * all connections.
         */
        stopContinuousFailover({waitForPrimary: waitForPrimary = false} = {}) {
            if (!this._stepdownThread.hasStarted()) {
                return;
            }

            this._stepdownThread.stop();

            if (waitForPrimary) {
                this.getPrimary();
                this.nodes.forEach((node) => reconnect(node));
            }
        }
    };
}

/**
 * Overrides the ShardingTest constructor to start the continuous primary stepdown thread.
 */
function makeShardingTestWithContinuousPrimaryStepdown(stepdownOptions, verbositySetting) {
    return class ShardingTestWithContinuousPrimaryStepdown extends ShardingTest {
        constructor(params) {
            params.other = params.other || {};

            if (stepdownOptions.configStepdown) {
                params.other.configOptions = params.other.configOptions || {};
                params.other.configOptions.setParameter = params.other.configOptions.setParameter || {};
                params.other.configOptions.setParameter.logComponentVerbosity = verbositySetting;
            }

            if (stepdownOptions.shardStepdown) {
                params.other.rsOptions = params.other.rsOptions || {};
                params.other.rsOptions.setParameter = params.other.rsOptions.setParameter || {};
                params.other.rsOptions.setParameter.logComponentVerbosity = verbositySetting;
            }

            // Construct the original object.
            super(params);

            // Validate the stepdown options.
            if (stepdownOptions.configStepdown && !this.configRS) {
                throw new Error("Continuous config server primary step down only available with CSRS");
            }

            if (stepdownOptions.shardStepdown && this._rs.some((rst) => !rst)) {
                throw new Error("Continuous shard primary step down only available with replica set shards");
            }
        }

        /**
         * Calls startContinuousFailover on the config server and/or each shard replica set as
         * specifed by the stepdownOptions object.
         */
        startContinuousFailover() {
            if (stepdownOptions.configStepdown) {
                this.configRS.startContinuousFailover();
            }

            if (stepdownOptions.shardStepdown) {
                this._rs.forEach(function (rst) {
                    rst.test.startContinuousFailover();
                });
            }
        }

        /**
         * Calls stopContinuousFailover on the config server and each shard replica set as
         * specified by the stepdownOptions object.
         *
         * If waitForPrimary is true, blocks until each replica set has elected a primary.
         */
        stopContinuousFailover({waitForPrimary: waitForPrimary = false} = {}) {
            if (stepdownOptions.configStepdown) {
                this.configRS.stopContinuousFailover({waitForPrimary: waitForPrimary});
            }

            if (stepdownOptions.shardStepdown) {
                this._rs.forEach(function (rst) {
                    rst.test.stopContinuousFailover({waitForPrimary: waitForPrimary});
                });
            }
        }

        /**
         * This method is disabled because it runs aggregation, which doesn't handle config
         * server stepdown correctly.
         */
        printShardingStatus() {}
    };
}
