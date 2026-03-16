/**
 * Tests that session cache operations (refresh and reap) are marked as non-deprioritizable,
 * and this is reflected in the nonDeprioritizable admission statistics.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getTotalMarkedNonDeprioritizableCount} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";

// Disable implicit sessions so we can control session creation
TestData.disableImplicitSessions = true;

/**
 * Asserts that the totalMarkedNonDeprioritizable counter increases after running an operation.
 */
function assertNonDeprioritizableCountIncreases(node, operationFn, description) {
    const beforeCount = getTotalMarkedNonDeprioritizableCount(node);
    operationFn();
    const afterCount = getTotalMarkedNonDeprioritizableCount(node);

    assert.gt(afterCount, beforeCount, description + " Before: " + beforeCount + ", After: " + afterCount);

    return {beforeCount, afterCount};
}

/**
 * Logs the change in totalMarkedNonDeprioritizable counter after running an operation.
 * Use this when the counter change is expected but not strictly required (e.g., remote operations).
 */
function logNonDeprioritizableCountChange(node, operationFn, description) {
    const beforeCount = getTotalMarkedNonDeprioritizableCount(node);
    operationFn();
    const afterCount = getTotalMarkedNonDeprioritizableCount(node);

    jsTestLog(description + ": beforeCount=" + beforeCount + ", afterCount=" + afterCount);

    return {beforeCount, afterCount};
}

/**
 * Checks the totalMarkedNonDeprioritizable counter on multiple nodes after running an operation.
 */
function checkNonDeprioritizableCountOnMultipleNodes(nodeConfigs, operationFn) {
    const beforeCounts = {};
    for (const config of nodeConfigs) {
        beforeCounts[config.description] = getTotalMarkedNonDeprioritizableCount(config.node);
    }

    operationFn();

    const results = {};
    for (const config of nodeConfigs) {
        const afterCount = getTotalMarkedNonDeprioritizableCount(config.node);
        const beforeCount = beforeCounts[config.description];
        results[config.description] = {beforeCount, afterCount};

        if (config.assertIncrease) {
            assert.gt(
                afterCount,
                beforeCount,
                config.description + " count should increase. Before: " + beforeCount + ", After: " + afterCount,
            );
        }
    }

    return results;
}

describe("Session cache operations non-deprioritizable", function () {
    let st;
    let configPrimary;
    let shardPrimary;
    let shardSecondary;
    let mongos;

    before(function () {
        st = new ShardingTest({
            shards: 1,
            config: 1,
            rs: {nodes: 2}, // 2 nodes to have primary and secondary
            configOptions: {
                setParameter: {
                    executionControlDeprioritizationGate: true,
                },
            },
            shardOptions: {
                setParameter: {
                    executionControlDeprioritizationGate: true,
                },
            },
        });
        configPrimary = st.configRS.getPrimary();
        shardPrimary = st.rs0.getPrimary();
        shardSecondary = st.rs0.getSecondary();
        mongos = st.s;
    });

    after(function () {
        st.stop();
    });

    it("should mark config server session refresh as non-deprioritizable (local)", function () {
        // Create a session on config server
        assert.commandWorked(configPrimary.getDB("admin").runCommand({startSession: 1}));

        // Trigger session cache refresh - local operation since config.system.sessions
        // is co-located on the config server.
        assertNonDeprioritizableCountIncreases(
            configPrimary,
            () => assert.commandWorked(configPrimary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1})),
            "Config server session refresh",
        );
    });

    it("should mark config server session reap as non-deprioritizable (local)", function () {
        // Trigger session cache reap - local operation
        assertNonDeprioritizableCountIncreases(
            configPrimary,
            () => assert.commandWorked(configPrimary.adminCommand({reapLogicalSessionCacheNow: 1})),
            "Config server session reap",
        );
    });

    it("should mark mongos session refresh as non-deprioritizable on shard (remote)", function () {
        // Create a session through mongos so the refresh has work to do
        assert.commandWorked(mongos.getDB("admin").runCommand({startSession: 1}));

        // Trigger session cache refresh on mongos - writes to config.system.sessions
        // are routed to shards since the collection is sharded.
        logNonDeprioritizableCountChange(
            shardPrimary,
            () => assert.commandWorked(mongos.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1})),
            "Mongos session refresh (remote to shard)",
        );
    });

    it("should mark mongos session reap as non-deprioritizable on shard (remote)", function () {
        // Trigger session cache reap on mongos - reads from config.system.sessions
        // are routed to shards.
        logNonDeprioritizableCountChange(
            shardPrimary,
            () => assert.commandWorked(mongos.adminCommand({reapLogicalSessionCacheNow: 1})),
            "Mongos session reap (remote to shard)",
        );
    });

    it("should mark shard primary session refresh as non-deprioritizable (local)", function () {
        // Create a session on shard primary so the refresh has work to do
        assert.commandWorked(shardPrimary.getDB("admin").runCommand({startSession: 1}));

        // Trigger session cache refresh - applies ScopedTaskTypeNonDeprioritizable locally
        assertNonDeprioritizableCountIncreases(
            shardPrimary,
            () => assert.commandWorked(shardPrimary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1})),
            "Shard primary session refresh",
        );
    });

    it("should mark shard primary session reap as non-deprioritizable (local)", function () {
        // Trigger session cache reap - applies ScopedTaskTypeNonDeprioritizable locally
        assertNonDeprioritizableCountIncreases(
            shardPrimary,
            () => assert.commandWorked(shardPrimary.adminCommand({reapLogicalSessionCacheNow: 1})),
            "Shard primary session reap",
        );
    });

    it("should mark shard secondary session refresh as non-deprioritizable (local + remote to primary)", function () {
        // On shard secondary:
        // 1. ScopedTaskTypeNonDeprioritizable is applied locally on the secondary
        // 2. Session writes route through cluster::write to the shard primary
        const results = checkNonDeprioritizableCountOnMultipleNodes(
            [
                {node: shardSecondary, description: "Shard secondary", assertIncrease: true},
                {node: shardPrimary, description: "Shard primary", assertIncrease: false},
            ],
            () => assert.commandWorked(shardSecondary.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1})),
        );

        jsTestLog(
            "Shard secondary refresh: secondary before=" +
                results["Shard secondary"].beforeCount +
                ", after=" +
                results["Shard secondary"].afterCount +
                "; primary before=" +
                results["Shard primary"].beforeCount +
                ", after=" +
                results["Shard primary"].afterCount,
        );
    });

    it("should mark shard secondary session reap as non-deprioritizable (local + remote to primary)", function () {
        // On shard secondary:
        // 1. ScopedTaskTypeNonDeprioritizable is applied locally on the secondary
        // 2. Session reads route through ClusterFind::runQuery
        const results = checkNonDeprioritizableCountOnMultipleNodes(
            [
                {node: shardSecondary, description: "Shard secondary", assertIncrease: true},
                {node: shardPrimary, description: "Shard primary", assertIncrease: false},
            ],
            () => assert.commandWorked(shardSecondary.adminCommand({reapLogicalSessionCacheNow: 1})),
        );

        jsTestLog(
            "Shard secondary reap: secondary before=" +
                results["Shard secondary"].beforeCount +
                ", after=" +
                results["Shard secondary"].afterCount +
                "; primary before=" +
                results["Shard primary"].beforeCount +
                ", after=" +
                results["Shard primary"].afterCount,
        );
    });
});
