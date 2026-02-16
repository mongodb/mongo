/**
 * Verifies the message size limits for pre-auth and post-auth commands are correctly enforced.
 */

function sendLargeHello(conn, shouldFail) {
    const largeHello = {hello: 1, bigField: "x".repeat(20 * 1024)};

    let ssNetworkMetricsBefore = connAdmin.runCommand({serverStatus: 1}).metrics.network;
    if (shouldFail) {
        let closedConnErr;
        try {
            conn.adminCommand(largeHello);
        } catch (e) {
            closedConnErr = e;
        }
        assert(closedConnErr, "Expected server to close the connection on oversized hello");
        assert(
            isNetworkError(closedConnErr),
            () => "Expected a network error from closed connection, got: " + tojson(closedConnErr),
        );
    } else {
        assert.commandWorked(conn.adminCommand(largeHello));
    }

    const expectedTotalMessageSizeErrorPreAuth = shouldFail
        ? Number(ssNetworkMetricsBefore.totalMessageSizeErrorPreAuth) + 1
        : Number(ssNetworkMetricsBefore.totalMessageSizeErrorPreAuth);
    let ssNetworkMetricsAfter = connAdmin.runCommand({serverStatus: 1}).metrics.network;
    assert.eq(expectedTotalMessageSizeErrorPreAuth,
              Number(ssNetworkMetricsAfter.totalMessageSizeErrorPreAuth));
    assert.eq(Number(ssNetworkMetricsAfter.totalMessageSizeErrorPostAuth), 0);
}

const rsName = jsTestName();

const rs = new ReplSetTest({name: rsName, nodes: 1, keyFile: "jstests/libs/key1"});

rs.startSet({
    setParameter: {
        messageSizeErrorRateSec: 5,
    },
});
rs.initiate();

const primary = rs.getPrimary();
const adminDB = primary.getDB("admin");

const user = jsTestName() + "_admin";
const pwd = "pwd";
assert.commandWorked(
    adminDB.runCommand({
        createUser: user,
        pwd: pwd,
        roles: ["root"],
        writeConcern: {w: "majority"},
    }),
);

const conn = new Mongo(primary.host);
const connAdmin = conn.getDB("admin");
assert.eq(1, connAdmin.auth(user, pwd), "Authentication failed");

{
    const newConn = new Mongo(primary.host);
    sendLargeHello(newConn, true);
}

sendLargeHello(conn, false);

assert.commandWorked(
    connAdmin.runCommand({
        setParameter: 1,
        preAuthMaximumMessageSizeBytes: 1024 * 1024,
    }),
    "Failed to set preAuthMaximumMessageSizeBytes to 1MB",
);

{
    const newConn = new Mongo(primary.host);
    sendLargeHello(newConn, false);
    newConn.close();
}

conn.close();
rs.stopSet();
