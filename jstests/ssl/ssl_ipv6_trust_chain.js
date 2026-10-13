/**
 * Verifies TLS chain-of-trust enforcement for connections over IPv6: a trusted certificate is
 * accepted, an untrusted chain is rejected, and tlsAllowInvalidCertificates bypasses the failure.
 */
const SERVER_CERT = "jstests/libs/server_SAN.pem"; // SAN includes IP Address ::1, signed by ca.pem
const CLIENT_CERT = "jstests/libs/client.pem";
const CA_CERT = "jstests/libs/ca.pem"; // the CA that actually signed SERVER_CERT
const WRONG_CA_CERT = "jstests/libs/trusted-ca.pem"; // a different CA -> chain must not validate

// Connect a shell to `host` over IPv6, trusting `caFile`, and return the process exit code.
// `extraArgs` appends additional client flags (e.g. --tlsAllowInvalidCertificates).
function connectWithCA(host, port, caFile, extraArgs = []) {
    return runMongoProgram(
        "mongo",
        "--host",
        host,
        "--port",
        port,
        "--ipv6",
        "--tls",
        "--tlsCAFile",
        caFile,
        "--tlsCertificateKeyFile",
        CLIENT_CERT,
        ...extraArgs,
        "--eval",
        ";",
    );
}

const mongod = MongoRunner.runMongod({
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: SERVER_CERT,
    tlsCAFile: CA_CERT,
    ipv6: "",
    bind_ip: "::1,127.0.0.1",
});

// Some test machines lack IPv6; runMongod returns null when it cannot bind ::1.
if (mongod == null) {
    jsTestLog("IPv6 is not available on this machine; skipping.");
} else {
    // Positive control: the client trusts the CA that signed the server certificate and the
    // server's SAN lists ::1, so the connection succeeds.
    assert.eq(
        0,
        connectWithCA("::1", mongod.port, CA_CERT),
        "IPv6 connection with a trusted, matching certificate should succeed",
    );

    // The server certificate lists ::1 in its SAN but is signed by a CA the client does not
    // trust, so chain-of-trust validation fails and the connection must be rejected.
    assert.neq(
        0,
        connectWithCA("::1", mongod.port, WRONG_CA_CERT),
        "IPv6 connection with an untrusted certificate chain must be rejected",
    );

    // tlsAllowInvalidCertificates downgrades certificate validation failures to a log warning, so
    // the same untrusted chain must be accepted when the flag is set.
    assert.eq(
        0,
        connectWithCA("::1", mongod.port, WRONG_CA_CERT, ["--tlsAllowInvalidCertificates"]),
        "tlsAllowInvalidCertificates should bypass an untrusted chain over IPv6",
    );

    MongoRunner.stopMongod(mongod);
}
