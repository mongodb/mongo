/**
 * Windows TLS 1.3 (Schannel) server-side post-handshake (KeyUpdate) test.
 *
 * The KMIP-based ESE tests only exercise mongod as a TLS *client* receiving a
 * NewSessionTicket, which drives the client-side post-handshake path
 * (InitializeSecurityContext).  The server-side path (AcceptSecurityContext, taken when
 * `_isClient == false` in SSLReadManager::processPostHandshakeToken) is never reached by
 * the rest of the suite, because NewSessionTickets flow server->client and the Schannel
 * shell client does not send KeyUpdates.
 *
 * This test closes that gap: it connects to mongod with `openssl s_client` and sends a TLS
 * 1.3 KeyUpdate (update_requested), which mongod-as-server must process via
 * AcceptSecurityContext and then continue decrypting subsequent application data.  A
 * KeyUpdate with update_requested also forces mongod to emit its own KeyUpdate response,
 * exercising the ISC/ASC output path.
 *
 * Pass criteria (asserted from the mongod log):
 *   1. The server-side post-handshake path ran (log 12786300 with isClient:false).
 *   2. No DecryptMessage failure (log 7998027 / SEC_E_DECRYPT_FAILURE) occurred — i.e. the
 *      KeyUpdate did not corrupt the application-layer traffic keys.
 */
import {before, after, describe, it} from "jstests/libs/mochalite.js";
import {determineSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";
import {windowsSupportsTLS13} from "jstests/libs/os_helpers.js";
import {getPython3Binary} from "jstests/libs/python.js";

if (determineSSLProvider() !== "windows" || !windowsSupportsTLS13()) {
    jsTest.log.info("Skipping tls13_windows_keyupdate.js: requires Windows with TLS 1.3 support.");
    quit();
}

// Permanent log emitted by SSLReadManager::processPostHandshakeToken(); the test depends on
// this id and on its `isClient` attribute remaining stable (emitted at debug level 2).
const kPostHandshakeResultLogId = 12786300;
// Emitted by decryptBuffer() when DecryptMessage fails; this status indicates key corruption.
const kDecryptFailedLogId = 7998027;
const kSecEDecryptFailure = -2146893008; // 0x80090330 as int32.

describe("Windows TLS 1.3 server-side KeyUpdate handling", function () {
    let mongod;

    before(function () {
        mongod = MongoRunner.runMongod({
            tlsMode: "requireTLS",
            tlsCertificateKeyFile: getX509Path("server.pem"),
            tlsCAFile: getX509Path("ca.pem"),
            // The openssl client connects without a client certificate.
            tlsAllowConnectionsWithoutCertificates: "",
            // Force TLS 1.3 so the post-handshake KeyUpdate path is exercised.
            tlsDisabledProtocols: "TLS1_0,TLS1_1,TLS1_2",
        });
        assert.neq(null, mongod, "mongod failed to start");

        // processPostHandshakeToken logs its result at debug level 2; raise network verbosity
        // so the entry is emitted into the global log for checkLog to read.
        assert.commandWorked(mongod.adminCommand({setParameter: 1, logComponentVerbosity: {network: {verbosity: 2}}}));
    });

    after(function () {
        if (mongod) {
            MongoRunner.stopMongod(mongod);
        }
    });

    it("processes a client KeyUpdate via AcceptSecurityContext without corrupting keys", function () {
        const rc = runProgram(
            getPython3Binary(),
            "jstests/ssl/libs/tls13_keyupdate_client.py",
            "--host",
            "127.0.0.1",
            "--port",
            "" + mongod.port,
        );

        if (rc === 2) {
            jsTest.log.info("Skipping assertions: openssl CLI unavailable or too old to send a KeyUpdate.");
            return;
        }
        assert.eq(0, rc, "TLS 1.3 KeyUpdate client exited with an unexpected code");

        // (1) The server-side post-handshake path (AcceptSecurityContext) must have run.
        assert.soon(
            () => checkLog.checkContainsOnceJson(mongod, kPostHandshakeResultLogId, {isClient: false}),
            "Expected server-side post-handshake (ASC) result log " +
                kPostHandshakeResultLogId +
                " with isClient:false — KeyUpdate was not processed by AcceptSecurityContext",
        );

        // (2) The KeyUpdate must not have corrupted the traffic keys: no decrypt failure.
        const globalLog = assert.commandWorked(mongod.adminCommand({getLog: "global"})).log;
        const sawDecryptFailure = globalLog.some(
            (line) => line.includes('"id":' + kDecryptFailedLogId) && line.includes("" + kSecEDecryptFailure),
        );
        assert(
            !sawDecryptFailure,
            "Found SEC_E_DECRYPT_FAILURE (log " +
                kDecryptFailedLogId +
                ") after the KeyUpdate — the traffic keys were corrupted",
        );
    });
});
