// Tests using a server parameter to set `ingressConnectionEstablishmentRateLimiterBypass` at
// runtime.

// TODO (SERVER-96103): Allow test to run on Windows.
if (_isWindows()) {
    quit();
}

const maxEstablishing = "ingressConnectionEstablishmentRateLimiterBypass";

function runTest(args, testFunc) {
    // Run tests in isolation to make sure we always start with a clean slate.
    var mongo = MongoRunner.runMongod(args);
    testFunc(mongo, maxEstablishing);
    MongoRunner.stopMongod(mongo);
}

function getParameter(conn, param) {
    const ret = conn.adminCommand({getParameter: 1, [param]: 1});
    return ret[param];
}

function setParameter(conn, param, newValue) {
    conn.adminCommand({setParameter: 1, [param]: newValue});
}

function setParameterAndVerify(conn, param, newValue) {
    setParameter(conn, param, newValue);
    assert.eq(getParameter(conn, param), newValue);
}

/**
 * Verify that there are no exemptions set by default, and retrieving that works.
 */
runTest({}, function(conn, param) {
    assert.eq(getParameter(conn, param).ranges, []);
});

/**
 * Reset the list of exemptions and then clear it out -- verify that both operations succeed.
 */
runTest({}, function(conn, param) {
    setParameterAndVerify(conn, param, {ranges: ["localhost"]});
    setParameterAndVerify(conn, param, {ranges: []});
});

/**
 * Verify that passing a mix of CIDR and HostAndPort ranges work.
 */
runTest({}, function(conn, param) {
    const ranges = {ranges: ["127.0.0.1/8", "/tmp/mongodb.sock", "8.8.8.8/8", "localhost"]};
    setParameterAndVerify(conn, param, ranges);
});

/**
 * Verify the behavior of the server parameters when set at startup, and then can be modified at
 * runtime.
 */
runTest({setParameter: {[maxEstablishing]: {ranges: ["127.0.0.1"]}}}, function(conn, param) {
    assert.eq(getParameter(conn, param).ranges, ["127.0.0.1/32"]);
    setParameterAndVerify(conn, param, {ranges: ["localhost"]});
});
