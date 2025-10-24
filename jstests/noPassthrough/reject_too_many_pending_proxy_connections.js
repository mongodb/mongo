/**
 * @tags: [
 *    requires_fcv_80,
 *    grpc_incompatible,
 * ]
 */

if (_isWindows()) {
    quit();
}

function verifyNumberOfRejectedProxyConnections(conn, expected) {
    const admin = conn.getDB("admin");
    let observed = 0;
    assert.soon(() => {
        observed = assert.commandWorked(admin.runCommand({ serverStatus: 1 })).network
            .connsRejectedDueToMaxPendingProxyProtocolHeader;
        return observed == expected;
    }, `observed = ${observed}`);
}

const kMaxPendingConnLimit = 5;
const kProxyProtocolTimeoutSecs = 120;
const listenPort = allocatePort();
const proxyPort = allocatePort();

// The ingress code is the same for both `mongos` and `mongod`, so we just test against a `mongod`.
const conn = MongoRunner.runMongod({
    port: listenPort, // port to listen for direct connections.
    proxyPort: proxyPort, // port to listen for incoming proxy connections.
    setParameter: {
        proxyProtocolMaximumPendingConnections: kMaxPendingConnLimit,
        proxyProtocolTimeoutSecs: kProxyProtocolTimeoutSecs,
    },
});

verifyNumberOfRejectedProxyConnections(conn, 0);

for (let i = 0; i < 2 * kMaxPendingConnLimit; i++) {
    assert.eq(0, runProgram("bash", "-c", `exec <>/dev/tcp/127.0.0.1/${proxyPort}`));
}

verifyNumberOfRejectedProxyConnections(conn, kMaxPendingConnLimit);

MongoRunner.stopMongod(conn);
