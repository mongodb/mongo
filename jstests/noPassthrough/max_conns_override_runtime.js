// Tests using a server parameter to set `maxIncomingConnectionsOverride` at runtime.

function runTest(args, testFunc) {
    // Run tests in isolation to make sure we always start with a clean slate.
    var mongo = MongoRunner.runMongod(args);
    testFunc(mongo);
    MongoRunner.stopMongod(mongo);
}

function setMaxIncomingConnectionsOverride(conn, newValue) {
    conn.adminCommand({setParameter: 1, maxIncomingConnectionsOverride: newValue});
}

function getMaxIncomingConnectionsOverride(conn) {
    const res = conn.adminCommand({getParameter: 1, maxIncomingConnectionsOverride: 1});
    return res.maxIncomingConnectionsOverride;
}

function setMaxIncomingConnectionsOverrideAndVerify(conn, newValue) {
    setMaxIncomingConnectionsOverride(conn, newValue);
    assert.eq(getMaxIncomingConnectionsOverride(conn), newValue);
}

/**
 * Verify that there are no exemptions set by default, and retrieving that works.
 */
runTest({}, function(conn) {
    assert.eq(getMaxIncomingConnectionsOverride(conn).ranges, []);
});

/**
 * Reset the list of exemptions and then clear it out -- verify that both operations succeed.
 */
runTest({}, function(conn) {
    setMaxIncomingConnectionsOverrideAndVerify(conn, {ranges: ["localhost"]});
    setMaxIncomingConnectionsOverrideAndVerify(conn, {ranges: []});
});

/**
 * Verify that passing a mix of CIDR and HostAndPort ranges work.
 */
runTest({}, function(conn) {
    const ranges = {ranges: ["127.0.0.1/8", "/tmp/mongodb.sock", "8.8.8.8/8", "localhost"]};
    setMaxIncomingConnectionsOverrideAndVerify(conn, ranges);
});

/**
 * Verify the behavior of the server parameter when `net.maxIncomingConnectionsOverride` is set at
 * startup, and then can be modified at runtime.
 */
runTest({config: "jstests/noPassthrough/libs/max_conns_override_config.yaml"}, function(conn) {
    assert.eq(getMaxIncomingConnectionsOverride(conn).ranges, ["127.0.0.1/32"]);
    setMaxIncomingConnectionsOverrideAndVerify(conn, {ranges: ["localhost"]});
});
