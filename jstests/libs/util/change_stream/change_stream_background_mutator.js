/**
 * BackgroundMutator module for running background operations during change stream tests.
 *
 * Responsibilities:
 * - Runs a configurable sequence of background operations in a loop.
 * - Waits for a stop signal from another instance via Connector.
 * - Applies random jitter to delays for realistic timing variation.
 * - Signals completion via Connector.notifyDone(instanceName).
 */
import "jstests/multiVersion/libs/multi_cluster.js";

import {binVersionToFCV} from "src/mongo/shell/feature_compatibility_version.js";
import {Connector} from "jstests/libs/util/change_stream/change_stream_connector.js";
import {Thread} from "jstests/libs/parallelTester.js";

/**
 * Background operation type constants.
 */
const BackgroundMutatorOpType = {
    ResetPlacementHistory: "resetPlacementHistory",
    FlipFCV: "flipFCV",
    FlipBinary: "flipBinary",
};

/**
 * BackgroundMutator class for running background operations during tests.
 */
class BackgroundMutator {
    /**
     * Default delay in milliseconds between downgrade and upgrade operations (FCV or binary).
     * Can be overridden via config.versionChangeDelayMs or by modifying this static property in tests.
     */
    static kDefaultVersionChangeDelayMs = 100;
    static kDefaultMinDelayMs = 2 * 1000;
    static kDefaultMaxDelayMs = 5 * 1000;
    static _threads = [];

    /**
     * Launch the background mutator in a background thread.
     * @param {Mongo} conn - MongoDB connection.
     * @param {Object} config - Configuration object containing:
     *   - instanceName: Name of the mutator instance (for signaling completion).
     *   - stopInstanceNames: Array of instance names; mutator stops when all are done.
     *   - ops: (Optional) Array of BackgroundMutatorOpType values. Defaults to all op types.
     *   - minDelayMs: (Optional) Minimum delay between operations (default: 2s).
     *   - maxDelayMs: (Optional) Maximum delay between operations (default: 5s).
     *   - versionChangeDelayMs: (Optional) Delay between downgrade and upgrade operations (default kDefaultVersionChangeDelayMs).
     *   - downgradeFCV: (Optional) FCV to downgrade to for FlipFCV (default: lastContinuousFCV).
     *   - shardingTest: (Optional) ShardingTest instance, required for FlipBinary.
     *   - oldBinaryVersion: (Optional) Old binary version to downgrade to for FlipBinary (e.g., "last-lts").
     */
    static start(conn, config) {
        const host = conn.host;
        const thread = new Thread(
            async function (host, config) {
                const {BackgroundMutator} = await import(
                    "jstests/libs/util/change_stream/change_stream_background_mutator.js"
                );
                const {Connector} = await import("jstests/libs/util/change_stream/change_stream_connector.js");
                const conn = new Mongo(host);
                try {
                    BackgroundMutator._execute(conn, config);
                } catch (e) {
                    jsTest.log.info("BackgroundMutator thread FAILED", {
                        instanceName: config.instanceName,
                        error: e.toString(),
                        stack: e.stack,
                    });
                    Connector.notifyDone(conn, config.instanceName);
                    throw e;
                }
            },
            host,
            config,
        );
        thread.start();
        BackgroundMutator._threads.push(thread);
    }

    static joinAll() {
        const threads = BackgroundMutator._threads;
        BackgroundMutator._threads = [];
        const errors = [];
        for (const t of threads) {
            try {
                t.join();
            } catch (e) {
                errors.push(e);
            }
        }
        if (errors.length > 0) {
            jsTest.log.error("BackgroundMutator threads failed", {errors});
            throw new Error(
                `${errors.length} BackgroundMutator thread(s) failed: ${errors.map((e) => e.toString()).join("; ")}`,
            );
        }
    }

    static _execute(conn, config) {
        const ops = config.ops || Object.values(BackgroundMutatorOpType);
        const stopNames = config.stopInstanceNames;
        assert(stopNames && stopNames.length > 0, "stopInstanceNames must be a non-empty array");
        const minDelayMs = config.minDelayMs || BackgroundMutator.kDefaultMinDelayMs;
        const maxDelayMs = config.maxDelayMs || BackgroundMutator.kDefaultMaxDelayMs;

        Random.setRandomSeed(config.seed);

        while (!BackgroundMutator._allDone(conn, stopNames)) {
            const opType = ops[Random.randInt(ops.length)];
            const delay = minDelayMs + Random.randInt(maxDelayMs - minDelayMs);

            jsTest.log.info(`BackgroundMutator [${config.instanceName}]: sleeping ${delay}ms before ${opType}`);
            sleep(delay);

            if (BackgroundMutator._allDone(conn, stopNames)) {
                break;
            }

            jsTest.log.info(`BackgroundMutator [${config.instanceName}]: running ${opType}`);
            BackgroundMutator._runOp(conn, opType, config);
        }

        jsTest.log.info(`BackgroundMutator [${config.instanceName}]: all writers done, stopping`);
        Connector.notifyDone(conn, config.instanceName);
    }

    static _allDone(conn, stopNames) {
        assert(stopNames && stopNames.length > 0, "stopNames must be a non-empty array");
        return stopNames.every((name) => Connector.isDone(conn, name));
    }

    /**
     * Execute a single background operation.
     * @param {Mongo} conn - MongoDB connection.
     * @param {string} opType - BackgroundMutatorOpType value.
     * @param {Object} config - Full configuration object.
     * @private
     */
    static _runOp(conn, opType, config) {
        switch (opType) {
            case BackgroundMutatorOpType.ResetPlacementHistory:
                BackgroundMutator._resetPlacementHistory(conn);
                break;
            case BackgroundMutatorOpType.FlipFCV:
                BackgroundMutator._flipFCV(conn, config);
                break;
            case BackgroundMutatorOpType.FlipBinary:
                BackgroundMutator._flipBinary(conn, config);
                break;
            default:
                throw new Error("Unknown BackgroundMutatorOpType: " + opType);
        }
    }

    /**
     * Reset placement history on the cluster.
     * @param {Mongo} conn - MongoDB connection (should be mongos or config server).
     * @private
     */
    static _resetPlacementHistory(conn) {
        try {
            assert.commandWorked(conn.getDB("admin").runCommand({resetPlacementHistory: 1}));
        } catch (e) {
            // resetPlacementHistory requires latestFCV. If FCV is downgraded or mid-transition
            // (e.g. because _flipFCV just ran), skip this iteration; the loop will retry later.
            if (e.code === ErrorCodes.CommandNotSupported) {
                jsTest.log.info("BackgroundMutator: resetPlacementHistory skipped (FCV not at latest)", {
                    errmsg: e.message,
                });
                return;
            }
            throw e;
        }
    }

    /**
     * Flip FCV: downgrade to the configured FCV, wait, then upgrade back to latestFCV.
     * Following the pattern from jstests/hooks/run_fcv_upgrade_downgrade_background.js,
     * we wait between operations to let the system settle in each FCV state.
     * @param {Mongo} conn - MongoDB connection.
     * @param {Object} config - Configuration object containing:
     *   - downgradeFCV: (Optional) FCV to downgrade to (default: lastContinuousFCV).
     *   - versionChangeDelayMs: (Optional) Delay between operations (default: kDefaultVersionChangeDelayMs).
     * @private
     */
    static _flipFCV(conn, config) {
        const delayMs = config.versionChangeDelayMs || BackgroundMutator.kDefaultVersionChangeDelayMs;
        // Default to lastContinuousFCV: it is always exactly one step below latestFCV, so the
        // downgrade is always a valid single-step transition. Callers may pass downgradeFCV
        // explicitly if they need a specific target (e.g. lastLTSFCV), but must then ensure the
        // transition is reachable from latestFCV in one step.
        const downgradeFCV = config.downgradeFCV || lastContinuousFCV;

        // Downgrade to the configured FCV.
        assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: downgradeFCV, confirm: true}));

        // Wait for the system to settle in the downgraded state.
        sleep(delayMs);

        // Upgrade back to latestFCV.
        assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    }

    /**
     * Flip binary versions: downgrade cluster to old binary, wait, then upgrade back to latest.
     * Uses st.downgradeCluster() and st.upgradeCluster() from jstests/multiVersion/libs/multi_cluster.js.
     * Requires shardingTest and oldBinaryVersion to be set in config.
     * @param {Mongo} conn - MongoDB connection (unused, passed for interface consistency).
     * @param {Object} config - Configuration with shardingTest, oldBinaryVersion, and optional versionChangeDelayMs.
     * @private
     */
    static _flipBinary(conn, config) {
        assert(config.shardingTest, "FlipBinary requires shardingTest in config");
        assert(config.oldBinaryVersion, "FlipBinary requires oldBinaryVersion in config");

        const st = config.shardingTest;
        const oldBinaryVersion = config.oldBinaryVersion;
        const delayMs = config.versionChangeDelayMs || BackgroundMutator.kDefaultVersionChangeDelayMs;

        // Phase 1: Downgrade FCV first (required before binary downgrade).
        // Use binVersionToFCV to get the appropriate FCV for the target binary version.
        const downgradeFCV = binVersionToFCV(oldBinaryVersion);
        assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: downgradeFCV, confirm: true}));

        // Phase 2: Downgrade binary (mongos first, then shards, then config servers).
        st.downgradeCluster(oldBinaryVersion, {waitUntilStable: true});

        // Wait for the system to operate in the downgraded state.
        sleep(delayMs);

        // Phase 3: Upgrade binary (config servers first, then shards, then mongos).
        st.upgradeCluster("latest", {waitUntilStable: true});

        // Phase 4: Upgrade FCV (after binary upgrade).
        assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    }
}

export {BackgroundMutator, BackgroundMutatorOpType};
