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
function testKeyRefreshWithFailpoint(triggerNode, counterNode, refreshIntervalMs, description) {
    const fastRefreshFp = configureFailPoint(triggerNode, "maxKeyRefreshWaitTimeOverrideMS", {
        overrideMS: refreshIntervalMs,
    });

    const beforeCount = getTotalMarkedNonDeprioritizableCount(counterNode);

    let afterCount;
    assert.soon(
        function () {
            afterCount = getTotalMarkedNonDeprioritizableCount(counterNode);
            return afterCount > beforeCount;
        },
        description + " - counter should increase. Before: " + beforeCount,
        30000,
        refreshIntervalMs,
    );

    fastRefreshFp.off();

    jsTestLog(description + ": beforeCount=" + beforeCount + ", afterCount=" + afterCount);

    assert.gt(
        afterCount,
        beforeCount,
        description +
            " - counter should increase. Before: " +
            beforeCount +
            ", After: " +
            afterCount,
    );

    return {beforeCount, afterCount};
}

/**
 * Tests that key generation on the config server primary causes the counter to increase.
 * Forces key generation by stepping down, then deleting keys on the new primary while
 * generation is still disabled so leftover keys cannot be included in beforeCount.
 */
function testKeyGenerationWithStepdown(st, configPrimary, keyPurpose, description) {
    // Pause generation on every config node so stepup cannot insert keys
    // before we snapshot the counter.
    for (let node of st.configRS.nodes) {
        assert.commandWorked(
            node.adminCommand({configureFailPoint: "disableKeyGeneration", mode: "alwaysOn"}),
        );
    }

    // Re-initialize the KeyGenerator on stepup. This fixture uses a 1-node config
    // replica set, so the same node is re-elected.
    assert.commandWorked(configPrimary.adminCommand({replSetStepDown: 5, force: true}));
    st.configRS.awaitNodesAgreeOnPrimary();
    configPrimary = st.configRS.getPrimary();
    const adminDb = configPrimary.getDB("admin");

    // Delete on the current primary while generation is still disabled.
    const keysBeforeDelete = adminDb.system.keys.find({purpose: keyPurpose}).toArray();
    jsTestLog(description + ": keys before deletion = " + keysBeforeDelete.length);

    const deleteResult = adminDb.system.keys.remove({purpose: keyPurpose});
    jsTestLog(description + ": deleted " + deleteResult.nRemoved + " " + keyPurpose + " keys");

    assert.soonNoExcept(
        () => adminDb.system.keys.find({purpose: keyPurpose}).itcount() === 0,
        "Expected " + keyPurpose + " keys to be 0 on the config primary before generation",
        30000,
        100,
    );

    const fastRefreshFp = configureFailPoint(configPrimary, "maxKeyRefreshWaitTimeOverrideMS", {
        overrideMS: 100,
    });

    const keysAtSnapshot = adminDb.system.keys.find({purpose: keyPurpose}).itcount();
    const beforeCount = getTotalMarkedNonDeprioritizableCount(configPrimary);
    jsTestLog(
        description +
            ": keys at beforeCount snapshot = " +
            keysAtSnapshot +
            ", beforeCount=" +
            beforeCount,
    );
    assert.eq(keysAtSnapshot, 0, description + " - HMAC keys must be 0 when beforeCount is taken");

    for (let node of st.configRS.nodes) {
        assert.commandWorked(
            node.adminCommand({configureFailPoint: "disableKeyGeneration", mode: "off"}),
        );
    }

    let afterCount;
    assert.soonNoExcept(
        function () {
            afterCount = getTotalMarkedNonDeprioritizableCount(configPrimary);
            const keyCount = adminDb.system.keys.find({purpose: keyPurpose}).itcount();
            return keyCount >= 2 && afterCount > beforeCount;
        },
        description +
            " - expected new " +
            keyPurpose +
            " keys and a counter increase. Before: " +
            beforeCount,
        30000,
        100,
    );
    fastRefreshFp.off();

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
        description +
            " - counter should increase. Before: " +
            beforeCount +
            ", After: " +
            afterCount,
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
        testKeyRefreshWithFailpoint(
            configPrimary,
            configPrimary,
            100,
            "Config server local key refresh",
        );
    });

    it("should mark mongos key refresh as non-deprioritizable on config server (remote)", function () {
        // When mongos's KeysCollectionManager refreshes keys, it queries the config server
        // as an internal client.
        testKeyRefreshWithFailpoint(
            mongos,
            configPrimary,
            50,
            "Mongos key refresh (remote to config)",
        );
    });

    it("should mark shard primary key refresh as non-deprioritizable on config server (remote)", function () {
        // When the shard primary's KeysCollectionManager refreshes keys, it queries the
        // config server as an internal client.
        testKeyRefreshWithFailpoint(
            shardPrimary,
            configPrimary,
            50,
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
            "Shard secondary key refresh (remote to config)",
        );
    });

    it("should mark config server's key generation as non-deprioritizable (local write)", function () {
        // Key generation (writes to admin.system.keys) only happens on the config server
        // primary.
        const result = testKeyGenerationWithStepdown(
            st,
            configPrimary,
            "HMAC",
            "Config server HMAC key generation",
        );

        // Update configPrimary reference since it may have changed after stepdown
        configPrimary = result.configPrimary;
    });
});
