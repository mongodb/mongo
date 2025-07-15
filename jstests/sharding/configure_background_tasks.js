/*
 * Tests configureBackgroundTask command.
 *
 * @tags: [
 *   requires_fcv_82,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});
const dbName = jsTestName();

(function testInvalidParameters() {
    jsTestLog("Testing invalid parameters for configureBackgroundTask command");

    assert.commandFailedWithCode(
        st.shard0.adminCommand({configureBackgroundTask: 1, task: "invalidTask", mode: "enabled"}),
        ErrorCodes.BadValue);

    assert.commandFailedWithCode(
        st.shard0.adminCommand(
            {configureBackgroundTask: 1, task: "ttlMonitor", mode: "invalidMode"}),
        ErrorCodes.BadValue);

    assert.commandFailedWithCode(
        st.shard0.getDB(dbName).runCommand(
            {configureBackgroundTask: 1, task: "ttlMonitor", mode: "enabled"}),
        ErrorCodes.Unauthorized);

    assert.commandFailedWithCode(st.shard0.adminCommand({
        configureBackgroundTask: 1,
        task: "ttlMonitor",
        mode: "enabled",
        throttleDelayMs: "invalidDelay"
    }),
                                 ErrorCodes.TypeMismatch);

    assert.commandFailedWithCode(st.shard0.adminCommand({
        configureBackgroundTask: 1,
        task: "ttlMonitor",
        mode: "throttled",
        throttleDelayMs: 10.5
    }),
                                 ErrorCodes.TypeMismatch);

    assert.commandFailedWithCode(st.shard0.adminCommand({
        configureBackgroundTask: 1,
        task: "ttlMonitor",
        mode: "throttled",
        throttleDelayMs: NumberInt(-100)
    }),
                                 ErrorCodes.IllegalOperation);
})();

(function testTTLMonitor() {
    jsTestLog("Testing TTL monitor task");

    assert.commandWorked(
        st.shard0.adminCommand({configureBackgroundTask: 1, task: "ttlMonitor", mode: "disabled"}));

    assert.commandWorked(
        st.shard0.adminCommand({configureBackgroundTask: 1, task: "ttlMonitor", mode: "enabled"}));

    assert.commandWorked(st.shard0.adminCommand({
        configureBackgroundTask: 1,
        task: "ttlMonitor",
        mode: "throttled",
        throttleDelayMs: NumberInt(500)
    }));
})();

(function testRangeDeleter() {
    jsTestLog("Testing range deleter task");

    assert.commandWorked(st.shard0.adminCommand(
        {configureBackgroundTask: 1, task: "rangeDeleter", mode: "enabled"}));

    assert.commandWorked(st.shard0.adminCommand(
        {configureBackgroundTask: 1, task: "rangeDeleter", mode: "disabled"}));

    assert.commandWorked(st.shard0.adminCommand({
        configureBackgroundTask: 1,
        task: "rangeDeleter",
        mode: "throttled",
        throttleDelayMs: NumberInt(500)
    }));
})();

(function testMigrations() {
    jsTestLog("Testing migrations task");

    assert.commandWorked(
        st.shard0.adminCommand({configureBackgroundTask: 1, task: "migrations", mode: "enabled"}));

    assert.commandWorked(
        st.shard0.adminCommand({configureBackgroundTask: 1, task: "migrations", mode: "disabled"}));

    assert.commandFailedWithCode(st.shard0.adminCommand({
        configureBackgroundTask: 1,
        task: "migrations",
        mode: "throttled",
        throttleDelayMs: NumberInt(500)
    }),
                                 ErrorCodes.IllegalOperation);
})();

st.stop();
