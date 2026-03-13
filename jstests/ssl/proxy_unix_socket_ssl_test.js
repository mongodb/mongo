/**
 * Verifies proxy-unix-socket ingress behavior with TLS enabled:
 * - A non-TLS client connection routed through a PROXY v2 server succeeds.
 * - A TLS client connection routed through the same path fails because TLS hello is rejected
 *   on proxy unix sockets.
 *
 * @tags: [
 *   grpc_incompatible,
 * ]
 */

if (_isWindows()) {
    quit();
}

import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";

const pemKeyFile = "jstests/libs/server.pem";
const caFile = "jstests/libs/ca.pem";
const prefix = `/tmp/${jsTestName()}_${Date.now()}`;
mkdir(prefix);

const mongod = MongoRunner.runMongod({
    proxyUnixSocketPrefix: prefix,
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: pemKeyFile,
    tlsCAFile: caFile,
    tlsAllowInvalidCertificates: "",
    setParameter: {
        logComponentVerbosity: {network: {verbosity: 4}},
    },
});
assert.neq(mongod, null, "Expected mongod to start");

const proxySocketPath = `${prefix}/proxy-mongodb-${mongod.port}.sock`;
assert(fileExists(proxySocketPath), `Expected proxy socket to exist: ${proxySocketPath}`);

const ingressPort = allocatePort();
const proxyServer = new ProxyProtocolServer(
    ingressPort,
    "" /* egressPort (ignored for unix egress) */,
    2 /* proxy protocol version */,
    {egressUnixSocket: proxySocketPath},
);
proxyServer.setTLVs([{type: 0x02, value: "authority.example.com"}]);
proxyServer.start();

try {
    // should allow a non-TLS client over proxy unix socket
    {
        const evalScript =
            `
const adminDB = db.getSiblingDB("admin");
assert.commandWorked(adminDB.runCommand({ping: 1}));
`;
        const res = runMongoProgram(
            "mongo", "--host", "127.0.0.1", "--port", ingressPort, "--eval", evalScript);
        assert.eq(res, 0, "Non-TLS connection through proxy unix socket should succeed");
    }

    // should reject a TLS client over proxy unix socket
    {
        const evalScript =
            `
const adminDB = db.getSiblingDB("admin");
assert.commandWorked(adminDB.runCommand({ping: 1}));
`;

        const res = runMongoProgram("mongo",
                                    "--host",
                                    "127.0.0.1",
                                    "--port",
                                    ingressPort,
                                    "--tls",
                                    "--tlsAllowInvalidCertificates",
                                    "--tlsCAFile",
                                    caFile,
                                    "--tlsCertificateKeyFile",
                                    pemKeyFile,
                                    "--eval",
                                    evalScript);
        assert.neq(res, 0, "TLS connection through proxy unix socket should fail");

        // should log the dedicated TLS-on-proxy-socket error
        checkLog.containsRelaxedJson(
            mongod,
            22988,
            {
                error: {
                    codeName: "SSLHandshakeFailed",
                    errmsg: "TLS hello received on proxy protocol unix socket",
                },
            },
            1,
            30 * 1000,
            (actual, expected) => actual >= expected,
        );
    }
} finally {
    proxyServer.stop();
    MongoRunner.stopMongod(mongod);
}
