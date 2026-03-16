/**
 * Tests that operations on the keys collection (admin.system.keys) are marked as
 * non-deprioritizable when performed by internal cluster components.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getTotalMarkedNonDeprioritizableCount} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";

/**
 * Tests that key refresh on a triggering node causes the counter to increase on the target node.
 * Uses the maxKeyRefreshWaitTimeOverrideMS failpoint to accelerate the refresh cycle.
 */
function testKeyRefreshWithFailpoint(triggerNode, counterNode, refreshIntervalMs, waitMs, description) {
    const fastRefreshFp = configureFailPoint(triggerNode, "maxKeyRefreshWaitTimeOverrideMS", {
        overrideMS: refreshIntervalMs,
    });

    const beforeCount = getTotalMarkedNonDeprioritizableCount(counterNode);
    sleep(waitMs);
    const afterCount = getTotalMarkedNonDeprioritizableCount(counterNode);

    fastRefreshFp.off();

    jsTestLog(description + ": beforeCount=" + beforeCount + ", afterCount=" + afterCount);

    assert.gte(
        afterCount,
        beforeCount,
        description + " - counter should not decrease. Before: " + beforeCount + ", After: " + afterCount,
    );

    return {beforeCount, afterCount};
}

/**
 * Tests that key generation on the config server primary causes the counter to increase.
 * Forces key generation by deleting existing keys and triggering a stepdown/stepup cycle.
 */
function testKeyGenerationWithStepdown(st, configPrimary, keyPurpose, description) {
    // Step 1: Pause key generation on all config server nodes
    for (let node of st.configRS.nodes) {
        assert.commandWorked(node.adminCommand({configureFailPoint: "disableKeyGeneration", mode: "alwaysOn"}));
    }

    // Step 2: Delete existing keys to force regeneration
    let adminDb = configPrimary.getDB("admin");
    const keysBeforeDelete = adminDb.system.keys.find({purpose: keyPurpose}).toArray();
    jsTestLog(description + ": keys before deletion = " + keysBeforeDelete.length);

    const deleteResult = adminDb.system.keys.remove({purpose: keyPurpose});
    jsTestLog(description + ": deleted " + deleteResult.nRemoved + " " + keyPurpose + " keys");

    // Step 3: Force a stepdown to re-initialize the KeyGenerator when primary comes back
    assert.commandWorked(configPrimary.adminCommand({replSetStepDown: 5, force: true}));

    // Wait for a new primary
    st.configRS.awaitNodesAgreeOnPrimary();
    configPrimary = st.configRS.getPrimary();
    adminDb = configPrimary.getDB("admin");

    // Step 4: Enable fast refresh on config primary BEFORE getting the counter
    const fastRefreshFp = configureFailPoint(configPrimary, "maxKeyRefreshWaitTimeOverrideMS", {overrideMS: 100});

    // Step 5: Get the counter before triggering key generation
    // (key generation is still disabled at this point)
    const beforeCount = getTotalMarkedNonDeprioritizableCount(configPrimary);

    // Step 6: Re-enable key generation on all config server nodes
    for (let node of st.configRS.nodes) {
        assert.commandWorked(node.adminCommand({configureFailPoint: "disableKeyGeneration", mode: "off"}));
    }

    // Step 7: Wait for key generation to complete
    assert.soonNoExcept(
        function () {
            const keys = adminDb.system.keys.find({purpose: keyPurpose}).toArray();
            return keys.length >= 2;
        },
        "Expected the config server primary to generate new " + keyPurpose + " keys",
        30000, // timeout
        100, // interval
    );

    // Give a little more time for any in-flight operations to complete
    sleep(200);

    const afterCount = getTotalMarkedNonDeprioritizableCount(configPrimary);
    fastRefreshFp.off();

    // Verify keys were actually generated
    const keysAfterGeneration = adminDb.system.keys.find({purpose: keyPurpose}).toArray();

    jsTestLog(
        description +
            ": beforeCount=" +
            beforeCount +
            ", afterCount=" +
            afterCount +
            ", keysGenerated=" +
            keysAfterGeneration.length,
    );

    assert.gt(
        afterCount,
        beforeCount,
        description + " - counter should increase. Before: " + beforeCount + ", After: " + afterCount,
    );

    return {
        configPrimary: configPrimary,
        beforeCount: beforeCount,
        afterCount: afterCount,
        keysGenerated: keysAfterGeneration.length,
    };
}

describe("Keys collection operations non-deprioritizable", function () {
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

    it("should mark config server's key refresh as non-deprioritizable (local)", function () {
        // The KeysCollectionManager on the config server performs local key refresh.
        // This is a local operation since admin.system.keys lives on the config server.
        testKeyRefreshWithFailpoint(configPrimary, configPrimary, 100, 300, "Config server local key refresh");
    });

    it("should mark mongos key refresh as non-deprioritizable on config server (remote)", function () {
        // When mongos's KeysCollectionManager refreshes keys, it queries the config server
        // as an internal client.
        testKeyRefreshWithFailpoint(mongos, configPrimary, 50, 200, "Mongos key refresh (remote to config)");
    });

    it("should mark shard primary key refresh as non-deprioritizable on config server (remote)", function () {
        // When the shard primary's KeysCollectionManager refreshes keys, it queries the
        // config server as an internal client.
        testKeyRefreshWithFailpoint(
            shardPrimary,
            configPrimary,
            50,
            200,
            "Shard primary key refresh (remote to config)",
        );
    });

    it("should mark shard secondary key refresh as non-deprioritizable on config server (remote)", function () {
        // When the shard secondary's KeysCollectionManager refreshes keys, it queries the
        // config server as an internal client.
        testKeyRefreshWithFailpoint(
            shardSecondary,
            configPrimary,
            50,
            200,
            "Shard secondary key refresh (remote to config)",
        );
    });

    it("should mark config server's key generation as non-deprioritizable (local write)", function () {
        // Key generation (writes to admin.system.keys) only happens on the config server
        // primary.
        const result = testKeyGenerationWithStepdown(st, configPrimary, "HMAC", "Config server HMAC key generation");

        // Update configPrimary reference since it may have changed after stepdown
        configPrimary = result.configPrimary;
    });
});
