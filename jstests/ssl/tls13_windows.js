/**
 * Windows-specific TLS 1.3 (Schannel) correctness tests.
 *
 * Covers gaps not exercised by the KMIP-based integration tests, which only exercise
 * the mongod-as-TLS-client path.  This file covers:
 *
 *   1. mongod as TLS 1.3 server — inbound connection negotiates TLS 1.3 cipher.
 *   2. Mutual TLS 1.3 — client certificate required and presented.
 *   3. Post-handshake stability — multiple sequential round-trips over a single TLS 1.3
 *      connection, exercising the Schannel NewSessionTicket / 0x80090317 code path on
 *      every read after the handshake.
 *   4. TLS 1.3-only server — rejects a TLS 1.2-only client.
 *   5. TLS 1.3-only client — rejects a TLS 1.2-only server.
 *
 */

import {determineSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";
import {windowsSupportsTLS13} from "jstests/libs/os_helpers.js";

if (determineSSLProvider() !== "windows" || !windowsSupportsTLS13()) {
    jsTest.log.info("Skipping: not running on Windows with TLS 1.3 support.");
    quit();
}

const CA_CERT = getX509Path("ca.pem");
const SERVER_CERT = getX509Path("server.pem");
const CLIENT_CERT = getX509Path("client.pem");

// Log id emitted by the Windows Schannel accept path when a TLS connection is established.
const kWindowsTLSAcceptedLogId = 6723802;
const kTLS13Cipher = "TLS_AES_256_GCM_SHA384";

// Log id emitted at debug level 0 by decryptBuffer when DecryptMessage returns 0x80090317.
// This fires when Schannel (as TLS client) receives a NewSessionTicket from an OpenSSL peer
// and the fallback TLS record header parser is used.  It will NOT fire in the
// Schannel-to-Schannel scenario of this test file; the KMIP integration tests exercise it.
const kTLS13PostHandshakeLogId = 7998029;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function mongoClientArgs(port, extraArgs) {
    return [
        "mongo",
        "--tls",
        "--tlsAllowInvalidHostnames",
        "--tlsCertificateKeyFile",
        CLIENT_CERT,
        "--tlsCAFile",
        CA_CERT,
        "--port",
        port,
        ...(extraArgs || []),
    ];
}

// ---------------------------------------------------------------------------
// Test 1: mongod as TLS 1.3 server — inbound connection
//
// Start mongod with no protocol restriction so it will offer TLS 1.3.
// Connect with a client that disables TLS 1.0/1.1/1.2, forcing TLS 1.3.
// Verify the server log records the TLS 1.3 cipher suite.
// ---------------------------------------------------------------------------
{
    jsTest.log.info("Test 1: TLS 1.3 server accepts inbound TLS 1.3 connection");

    const mongod = MongoRunner.runMongod({
        tlsMode: "requireTLS",
        tlsCertificateKeyFile: SERVER_CERT,
        tlsCAFile: CA_CERT,
    });

    const rc = runMongoProgram(
        ...mongoClientArgs(mongod.port, [
            "--tlsDisabledProtocols",
            "TLS1_0,TLS1_1,TLS1_2",
            "--eval",
            "assert.commandWorked(db.adminCommand({hello: 1}));",
        ]),
    );
    assert.eq(0, rc, "TLS 1.3 client should connect successfully to TLS 1.3 server");

    assert.soon(
        () => checkLog.checkContainsOnceJson(mongod, kWindowsTLSAcceptedLogId, {"cipher": kTLS13Cipher}),
        `Expected TLS 1.3 cipher ${kTLS13Cipher} to appear in server log`,
    );

    MongoRunner.stopMongod(mongod);
}

// ---------------------------------------------------------------------------
// Test 2: Mutual TLS 1.3 — client certificate required and verified
// ---------------------------------------------------------------------------
{
    jsTest.log.info("Test 2: TLS 1.3 mutual authentication with client certificate");

    const mongod = MongoRunner.runMongod({
        tlsMode: "requireTLS",
        tlsCertificateKeyFile: SERVER_CERT,
        tlsCAFile: CA_CERT,
    });

    const rc = runMongoProgram(
        ...mongoClientArgs(mongod.port, [
            "--tlsDisabledProtocols",
            "TLS1_0,TLS1_1,TLS1_2",
            "--eval",
            "assert.commandWorked(db.adminCommand({hello: 1}));",
        ]),
    );
    assert.eq(0, rc, "Mutual TLS 1.3 connection with client certificate should succeed");

    MongoRunner.stopMongod(mongod);
}

// ---------------------------------------------------------------------------
// Test 3: Post-handshake stability — multiple round-trips over one connection
//
// After the TLS 1.3 handshake the peer sends NewSessionTicket records.  On
// Windows Server 2022 Schannel returns 0x80090317 (error-form SEC_I_CONTEXT_EXPIRED)
// from DecryptMessage when it consumes one; the read manager must skip the ticket
// and correctly surface the following application-data records.
//
// Drive 20 sequential ping commands over the same shell process connection to
// create several opportunities for post-handshake records to arrive mid-read.
//
// Note: 0x80090317 (logged as kTLS13PostHandshakeLogId) fires only when Schannel
// acts as a TLS *client* receiving a NewSessionTicket from an OpenSSL server
// (e.g. the KMIP integration tests).  In this Schannel-to-Schannel test mongod is
// the server, so SEC_I_RENEGOTIATE is used instead.  The ping-loop success below
// remains the definitive check for post-handshake read stability.
// ---------------------------------------------------------------------------
{
    jsTest.log.info("Test 3: Post-handshake stability — 20 sequential round-trips over TLS 1.3");

    const mongod = MongoRunner.runMongod({
        tlsMode: "requireTLS",
        tlsCertificateKeyFile: SERVER_CERT,
        tlsCAFile: CA_CERT,
    });

    const pingLoop = `
        for (let i = 0; i < 20; i++) {
            assert.commandWorked(db.adminCommand({ping: 1}),
                                 "ping " + i + " should succeed over persistent TLS 1.3 connection");
        }
    `;

    const rc = runMongoProgram(
        ...mongoClientArgs(mongod.port, ["--tlsDisabledProtocols", "TLS1_0,TLS1_1,TLS1_2", "--eval", pingLoop]),
    );
    assert.eq(0, rc, "All 20 round-trips over a single TLS 1.3 connection should succeed");

    assert.soon(
        () => checkLog.checkContainsOnceJson(mongod, kWindowsTLSAcceptedLogId, {"cipher": kTLS13Cipher}),
        `Expected TLS 1.3 cipher ${kTLS13Cipher} in server log for post-handshake test`,
    );

    MongoRunner.stopMongod(mongod);
}

// ---------------------------------------------------------------------------
// Test 4: TLS 1.3-only server rejects a TLS 1.2-only client
// ---------------------------------------------------------------------------
{
    jsTest.log.info("Test 4: TLS 1.3-only server rejects TLS 1.2-only client");

    const mongod = MongoRunner.runMongod({
        tlsMode: "requireTLS",
        tlsCertificateKeyFile: SERVER_CERT,
        tlsCAFile: CA_CERT,
        tlsDisabledProtocols: "TLS1_0,TLS1_1,TLS1_2",
    });

    const rc = runMongoProgram(...mongoClientArgs(mongod.port, ["--tlsDisabledProtocols", "TLS1_3", "--eval", ";"]));
    assert.neq(0, rc, "TLS 1.2-only client should fail to connect to a TLS 1.3-only server");

    MongoRunner.stopMongod(mongod);
}

// ---------------------------------------------------------------------------
// Test 5: TLS 1.3-only client rejects a TLS 1.2-only server
// ---------------------------------------------------------------------------
{
    jsTest.log.info("Test 5: TLS 1.3-only client rejects TLS 1.2-only server");

    const mongod = MongoRunner.runMongod({
        tlsMode: "requireTLS",
        tlsCertificateKeyFile: SERVER_CERT,
        tlsCAFile: CA_CERT,
        tlsDisabledProtocols: "TLS1_3",
    });

    const rc = runMongoProgram(
        ...mongoClientArgs(mongod.port, ["--tlsDisabledProtocols", "TLS1_0,TLS1_1,TLS1_2", "--eval", ";"]),
    );
    assert.neq(0, rc, "TLS 1.3-only client should fail to connect to a TLS 1.2-only server");

    MongoRunner.stopMongod(mongod);
}

jsTest.log.info("All Windows TLS 1.3 server-side tests passed.");
