/**
 * Tests that the minWaitForStreamingHelloMillis server parameter enforces a minimum timeout
 * for pre-auth streamable hello commands.
 * @tags: [requires_replication, requires_fcv_70]
 */
const replTest = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            minWaitForStreamingHelloMillis: 2000,
            abortStreamingHelloWithSmallTimeout: false,
        },
    },
});
replTest.startSet();
replTest.initiate();
const primary = replTest.getPrimary();
const db = primary.getDB("admin");

// Clamps maxAwaitTimeMS to the minimum when below the threshold for an unauthenticated client.
// An awaitable hello with maxAwaitTimeMS: 0 should be clamped to the minimum (2000ms), so it
// should take at least ~2000ms to return when the topology does not change.
{
    const res = assert.commandWorked(db.runCommand({hello: 1}));
    const topologyVersion = res.topologyVersion;
    const start = new Date();
    assert.commandWorked(
        db.runCommand({hello: 1, topologyVersion: topologyVersion, maxAwaitTimeMS: 0}));
    const elapsed = new Date() - start;
    assert.gte(elapsed,
               1800,
               "Expected command to wait at least 1800ms due to clamping, but it completed in " +
                   elapsed + "ms");
}

// Allows maxAwaitTimeMS at or above the minimum without clamping.
{
    const res = assert.commandWorked(db.runCommand({hello: 1}));
    const topologyVersion = res.topologyVersion;
    const start = new Date();
    assert.commandWorked(
        db.runCommand({hello: 1, topologyVersion: topologyVersion, maxAwaitTimeMS: 2000}));
    const elapsed = new Date() - start;
    assert.gte(elapsed,
               1800,
               "Expected command to wait at least 1800ms, but it completed in " + elapsed + "ms");
}

// Aborts when abortStreamingHelloWithSmallTimeout is true.
{
    assert.commandWorked(
        db.adminCommand({setParameter: 1, abortStreamingHelloWithSmallTimeout: true}));
    const res = assert.commandWorked(db.runCommand({hello: 1}));
    const topologyVersion = res.topologyVersion;
    assert.commandFailedWithCode(
        db.runCommand({hello: 1, topologyVersion: topologyVersion, maxAwaitTimeMS: 0}),
        ErrorCodes.InvalidOptions);
    assert.commandWorked(
        db.adminCommand({setParameter: 1, abortStreamingHelloWithSmallTimeout: false}));
}

// Can update minWaitForStreamingHelloMillis at runtime.
{
    assert.commandWorked(db.adminCommand({setParameter: 1, minWaitForStreamingHelloMillis: 500}));
    const res = assert.commandWorked(db.runCommand({hello: 1}));
    const topologyVersion = res.topologyVersion;
    const start = new Date();
    assert.commandWorked(
        db.runCommand({hello: 1, topologyVersion: topologyVersion, maxAwaitTimeMS: 0}));
    const elapsed = new Date() - start;
    assert.gte(elapsed,
               400,
               "Expected command to wait at least 400ms due to clamping, but it completed in " +
                   elapsed + "ms");
    assert.commandWorked(db.adminCommand({setParameter: 1, minWaitForStreamingHelloMillis: 2000}));
}

replTest.stopSet();
