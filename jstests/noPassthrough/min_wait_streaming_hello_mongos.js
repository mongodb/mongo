/**
 * Tests that the minWaitForStreamingHelloMillis server parameter enforces a minimum timeout
 * for pre-auth streamable hello commands on mongos.
 *
 * mongos serves streamable hello from its own implementation (cluster_hello_cmd.cpp, backed by
 * MongosTopologyCoordinator) rather than the mongod one, so it needs coverage independent of
 * jstests/noPassthrough/min_wait_streaming_hello.js.
 *
 * This test needs a binary that carries the mongos-side fix rather than a particular FCV, so it is
 * denylisted from multiversion suites via etc/backports_required_for_multiversion_tests.yml instead
 * of being tagged with requires_fcv_*.
 * @tags: [requires_sharding]
 */
const st = new ShardingTest({
    shards: 1,
    mongos: 1,
    other: {
        auth: "",
        mongosOptions: {
            setParameter: {
                minWaitForStreamingHelloMillis: 2000,
                abortStreamingHelloWithSmallTimeout: false,
            },
        },
    },
});
const db = st.s0.getDB("admin");

// Create the admin user via the localhost exception. db itself is never authenticated, so it
// keeps exercising the unauthenticated, pre-auth code path used by the tests below even though
// the cluster now requires auth for other commands.
db.createUser({user: "admin", pwd: "admin", roles: jsTest.adminUserRoles});

// Clamps maxAwaitTimeMS to the minimum when below the threshold for an unauthenticated client.
// An awaitable hello with maxAwaitTimeMS: 0 should be clamped to the minimum (2000ms), so it
// should take at least ~2000ms to return when the topology does not change.
{
    const res = assert.commandWorked(db.runCommand({hello: 1}));
    const topologyVersion = res.topologyVersion;
    const start = new Date();
    assert.commandWorked(
        db.runCommand({hello: 1, topologyVersion: topologyVersion, maxAwaitTimeMS: 0}),
    );
    const elapsed = new Date() - start;
    assert.gte(
        elapsed,
        1800,
        "Expected command to wait at least 1800ms due to clamping, but it completed in " + elapsed +
            "ms",
    );
}

// Allows maxAwaitTimeMS at or above the minimum without clamping.
{
    const res = assert.commandWorked(db.runCommand({hello: 1}));
    const topologyVersion = res.topologyVersion;
    const start = new Date();
    assert.commandWorked(
        db.runCommand({hello: 1, topologyVersion: topologyVersion, maxAwaitTimeMS: 2000}),
    );
    const elapsed = new Date() - start;
    assert.gte(
        elapsed,
        1800,
        "Expected command to wait at least 1800ms, but it completed in " + elapsed + "ms",
    );
}

// Aborts when abortStreamingHelloWithSmallTimeout is true.
{
    assert.commandWorked(
        db.adminCommand({setParameter: 1, abortStreamingHelloWithSmallTimeout: true}),
    );
    const res = assert.commandWorked(db.runCommand({hello: 1}));
    const topologyVersion = res.topologyVersion;
    assert.commandFailedWithCode(
        db.runCommand({hello: 1, topologyVersion: topologyVersion, maxAwaitTimeMS: 0}),
        ErrorCodes.InvalidOptions,
    );
    assert.commandWorked(
        db.adminCommand({setParameter: 1, abortStreamingHelloWithSmallTimeout: false}),
    );
}

// Can update minWaitForStreamingHelloMillis at runtime.
{
    assert.commandWorked(db.adminCommand({setParameter: 1, minWaitForStreamingHelloMillis: 500}));
    const res = assert.commandWorked(db.runCommand({hello: 1}));
    const topologyVersion = res.topologyVersion;
    const start = new Date();
    assert.commandWorked(
        db.runCommand({hello: 1, topologyVersion: topologyVersion, maxAwaitTimeMS: 0}),
    );
    const elapsed = new Date() - start;
    assert.gte(
        elapsed,
        400,
        "Expected command to wait at least 400ms due to clamping, but it completed in " + elapsed +
            "ms",
    );
    assert.commandWorked(db.adminCommand({setParameter: 1, minWaitForStreamingHelloMillis: 2000}));
}

// Does not clamp maxAwaitTimeMS for an authenticated client. Use a separate, dedicated
// connection so authenticating it does not affect db, which the blocks above rely on staying
// unauthenticated.
{
    const authenticatedConn = new Mongo(st.s0.host);
    const authenticatedDb = authenticatedConn.getDB("admin");
    assert(authenticatedDb.auth("admin", "admin"));

    const res = assert.commandWorked(authenticatedDb.runCommand({hello: 1}));
    const topologyVersion = res.topologyVersion;
    const start = new Date();
    assert.commandWorked(
        authenticatedDb.runCommand({hello: 1, topologyVersion: topologyVersion, maxAwaitTimeMS: 0}),
    );
    const elapsed = new Date() - start;
    assert.lt(
        elapsed,
        1800,
        "Expected an authenticated client's hello to return quickly, not be clamped, but it took " +
            elapsed + "ms",
    );
    authenticatedConn.close();
}

st.stop();
