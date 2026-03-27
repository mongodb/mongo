// Test that the client field in auth logs is emitting correctly.
import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";

// Test that the auth logs include the `expectedClient`.
function testClientAttr(mongodRunner, db, expectedClient, connectionHealthLoggingOn) {
    db.auth("admin", "pwd");
    if (connectionHealthLoggingOn) {
        assert.soon(
            () =>
                checkLog.checkContainsWithCountJson(
                    mongodRunner,
                    5286306,
                    {"client": expectedClient, "result": 0},
                    1,
                    null,
                    true,
                ),
            "Did not find expected sourceClient in successful auth log",
        );
    } else {
        assert.eq(
            checkLog.checkContainsOnceJson(mongodRunner, 5286306, {}),
            false,
            "Expected not to find successful auth log entry",
        );
    }
    db.logout();

    db.auth("admin", "wrong");
    if (connectionHealthLoggingOn) {
        assert.soon(
            () =>
                checkLog.checkContainsWithCountJson(
                    mongodRunner,
                    5286307,
                    {"client": expectedClient, "result": 18},
                    1,
                    null,
                    true,
                ),
            "Did not find expected sourceClient in failed auth log",
        );
    } else {
        assert.eq(
            checkLog.checkContainsOnceJson(mongodRunner, 5286307, {}),
            false,
            "Expected not to find failed auth log entry",
        );
    }
}

function runClientAttrTests(connectionHealthLoggingOn) {
    // This test is not supported on windows since it uses the ProxyProtocolServer.
    if (_isWindows()) {
        return;
    }

    const ingressPort = allocatePort();
    const prefix = `${MongoRunner.dataPath}${jsTestName()}`;
    mkdir(prefix);
    const mongod = MongoRunner.runMongod({
        proxyUnixSocketPrefix: prefix,
        unixSocketPrefix: prefix,
        setParameter: {
            enableDetailedConnectionHealthMetricLogLines: connectionHealthLoggingOn,
        },
    });

    const unixSocketPath = `${prefix}/mongodb-${mongod.port}.sock`;
    assert(fileExists(unixSocketPath), `Expected unix socket to exist: ${unixSocketPath}`);
    const proxySocketPath = `${prefix}/proxy-mongodb-${mongod.port}.sock`;
    assert(fileExists(proxySocketPath), `Expected proxy socket to exist: ${proxySocketPath}`);

    let proxyServer = new ProxyProtocolServer(ingressPort, mongod.port, 2, {
        egressUnixSocket: proxySocketPath,
    });
    proxyServer.setTLVs([{"type": 0x02, "value": "authority.example.com"}]);
    proxyServer.start();

    try {
        mongod.getDB("admin").createUser({user: "admin", pwd: "pwd", roles: ["root"], mechanisms: ["SCRAM-SHA-256"]});

        // Connection via standard unix socket should emit "anonymous unix socket:27017" as remote addr.
        const unixConn = new Mongo(unixSocketPath);
        const unixAdminDB = unixConn.getDB("admin");
        testClientAttr(mongod, unixAdminDB, "anonymous unix socket:27017", connectionHealthLoggingOn);

        // Connection via the proxy unix socket should emit the source ip address as remote addr.
        const proxiedConn = new Mongo(`mongodb://127.0.0.1:${ingressPort}`);
        testClientAttr(mongod, proxiedConn.getDB("admin"), /^127\.0\.0\.1:\d{1,5}$/, connectionHealthLoggingOn);
    } finally {
        MongoRunner.stopMongod(mongod);
        proxyServer.stop();
    }
}

runClientAttrTests(true);
runClientAttrTests(false);
