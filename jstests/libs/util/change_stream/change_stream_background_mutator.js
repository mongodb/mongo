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

import {binVersionToFCV} from "jstests/libs/feature_compatibility_version.js";
import {Connector} from "jstests/libs/util/change_stream/change_stream_connector.js";

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

    /**
     * Run the background mutator with the given configuration.
     * @param {Mongo} conn - MongoDB connection.
     * @param {Object} config - Configuration object containing:
     *   - instanceName: Name of the mutator instance (for signaling completion).
     *   - stopInstanceName: Name of the instance to watch for stop signal.
     *   - ops: Array of BackgroundMutatorOpType values to cycle through.
     *   - delayMs: Base delay between operations in milliseconds.
     *   - versionChangeDelayMs: (Optional) Delay between downgrade and upgrade operations (default kDefaultVersionChangeDelayMs).
     *   - downgradeFCV: (Optional) FCV to downgrade to for FlipFCV (default: lastLTSFCV).
     *   - shardingTest: (Optional) ShardingTest instance, required for FlipBinary.
     *   - oldBinaryVersion: (Optional) Old binary version to downgrade to for FlipBinary (e.g., "last-lts").
     */
    static run(conn, config) {
        const ops = config.ops;
        if (!ops.length) {
            Connector.notifyDone(conn, config.instanceName);
            return;
        }

        let index = 0;
        while (!Connector.isDone(conn, config.stopInstanceName)) {
            const opType = ops[index];

            const delay = BackgroundMutator._computeDelay(config.delayMs);
            sleep(delay);

            BackgroundMutator._runOp(conn, opType, config);

            index = (index + 1) % ops.length;
        }

        Connector.notifyDone(conn, config.instanceName);
    }

    /**
     * Apply small random jitter on top of the base delay.
     * @param {number} baseDelayMs - Base delay in milliseconds.
     * @returns {number} Delay with jitter applied (up to +25%).
     * @private
     */
    static _computeDelay(baseDelayMs) {
        if (baseDelayMs <= 0) {
            return 0;
        }
        return Math.floor(baseDelayMs * (1 + Math.random() * 0.25));
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
        assert.commandWorked(conn.getDB("admin").runCommand({resetPlacementHistory: 1}));
    }

    /**
     * Flip FCV: downgrade to the configured FCV, wait, then upgrade back to latestFCV.
     * Following the pattern from jstests/hooks/run_fcv_upgrade_downgrade_background.js,
     * we wait between operations to let the system settle in each FCV state.
     * @param {Mongo} conn - MongoDB connection.
     * @param {Object} config - Configuration object containing:
     *   - downgradeFCV: (Optional) FCV to downgrade to (default: lastLTSFCV).
     *   - versionChangeDelayMs: (Optional) Delay between operations (default: kDefaultVersionChangeDelayMs).
     * @private
     */
    static _flipFCV(conn, config) {
        const delayMs = config.versionChangeDelayMs || BackgroundMutator.kDefaultVersionChangeDelayMs;
        const downgradeFCV = config.downgradeFCV || lastLTSFCV;

        // Downgrade to the configured FCV.
        assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: downgradeFCV, confirm: true}));

        // Wait for the system to settle in the downgraded state.
        sleep(BackgroundMutator._computeDelay(delayMs));

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
        sleep(BackgroundMutator._computeDelay(delayMs));

        // Phase 3: Upgrade binary (config servers first, then shards, then mongos).
        st.upgradeCluster("latest", {waitUntilStable: true});

        // Phase 4: Upgrade FCV (after binary upgrade).
        assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    }
}

export {BackgroundMutator, BackgroundMutatorOpType};
