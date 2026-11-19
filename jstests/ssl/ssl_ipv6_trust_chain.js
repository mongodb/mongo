/**
 * Verifies TLS chain-of-trust enforcement for connections over IPv6: a trusted certificate is
 * accepted, an untrusted chain is rejected, and tlsAllowInvalidCertificates bypasses the failure.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const SERVER_CERT = getX509Path("server_SAN.pem"); // SAN includes IP Address ::1, signed by ca.pem
const CLIENT_CERT = getX509Path("client.pem");
const CA_CERT = getX509Path("ca.pem"); // the CA that actually signed SERVER_CERT
const WRONG_CA_CERT = getX509Path("trusted-ca.pem"); // a different CA -> chain must not validate

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

describe("TLS chain-of-trust enforcement over IPv6", function () {
    let mongod;

    before(function () {
        mongod = MongoRunner.runMongod({
            tlsMode: "requireTLS",
            tlsCertificateKeyFile: SERVER_CERT,
            tlsCAFile: CA_CERT,
            ipv6: "",
            bind_ip: "::1,127.0.0.1",
        });
        // Some test machines lack IPv6; runMongod returns null when it cannot bind ::1. Quit rather
        // than return early from each case, which mochalite would still report as passing.
        if (mongod == null) {
            jsTest.log.info("IPv6 is not available on this machine; skipping.");
            quit();
        }
    });

    after(function () {
        MongoRunner.stopMongod(mongod);
    });

    // Positive control: the client trusts the CA that signed the server certificate and the
    // server's SAN lists ::1, so the connection succeeds.
    it("accepts a trusted, matching certificate over IPv6", function () {
        assert.eq(
            0,
            connectWithCA("::1", mongod.port, CA_CERT),
            "IPv6 connection with a trusted, matching certificate should succeed",
        );
    });

    // The server certificate lists ::1 in its SAN but is signed by a CA the client does not
    // trust, so chain-of-trust validation fails and the connection must be rejected.
    it("rejects an untrusted certificate chain over IPv6", function () {
        assert.neq(
            0,
            connectWithCA("::1", mongod.port, WRONG_CA_CERT),
            "IPv6 connection with an untrusted certificate chain must be rejected",
        );
    });

    // tlsAllowInvalidCertificates downgrades certificate validation failures to a log warning, so
    // the same untrusted chain must be accepted when the flag is set.
    it("bypasses an untrusted chain over IPv6 with tlsAllowInvalidCertificates", function () {
        assert.eq(
            0,
            connectWithCA("::1", mongod.port, WRONG_CA_CERT, ["--tlsAllowInvalidCertificates"]),
            "tlsAllowInvalidCertificates should bypass an untrusted chain over IPv6",
        );
    });
});
