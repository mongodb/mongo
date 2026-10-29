/**
 * Tests that a `subject=` certificate selector on Windows skips an expired certificate and
 * selects another certificate that shares the same subject but is still valid.
 *
 * Regression test for SERVER-129942: when the Windows `My` store contains two certificates with
 * the same subject (e.g. an old expired cert left behind after renewal alongside the renewed
 * cert), MongoDB used to accept the first match returned by CertFindCertificateInStore. If that
 * first match was expired, mongod fatally failed to start instead of trying the valid duplicate.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getPython3Binary} from "jstests/libs/python.js";
import {
    requireSSLProvider,
    TRUSTED_CA_CERT,
    TRUSTED_CLIENT_CERT,
} from "jstests/ssl/libs/ssl_helpers.js";

// Both the expired and the valid certificate share this subject CN. CERT_FIND_SUBJECT_STR
// (used by CertFindCertificateInStore) searches the cert's simple display name, so the selector
// must be a substring of the CN — not a full X.500 DN.
const SHARED_SUBJECT = "Trusted Kernel Test Server";

// LOGV2 id emitted by SSLManagerWindows::_validateCertificate when the selected server
// certificate is expired or not yet valid. With the bug, selecting the expired duplicate causes
// mongod to fatal with this id during startup.
const EXPIRED_CERT_FATAL_ID = 50755;

// Identifies this test process as the holder of the host-wide cert store lock (see
// windows_castore_lock.py). Unique per resmoke invocation, so a release() call can never delete
// a lock file actually held by a different, concurrently running copy of this test.
const LOCK_TOKEN = `${Date.now()}-${Math.random()}`;

requireSSLProvider("windows", function () {
    describe("subject= certificate selector with a duplicate expired certificate", function () {
        before(function () {
            // Burn-in runs multiple copies of this same (new) test file in parallel on the same
            // host, and they all share one Windows cert store. windows_castore_cleanup.py below
            // deletes every MongoDB cert host-wide, so two copies running at once will delete
            // each other's certs mid-test. Take a host-wide lock for the whole cleanup+import+
            // mongod+verify+cleanup critical section (released in after()) so concurrent copies
            // are serialized instead of stomping on each other.
            assert.eq(
                0,
                runProgram(
                    getPython3Binary(),
                    "jstests/ssl_linear/windows_castore_lock.py",
                    "acquire",
                    LOCK_TOKEN,
                ),
                "failed to acquire the host-wide Windows cert store lock",
            );

            // Start from a clean slate so leftover MongoDB certs from prior runs don't interfere.
            assert.eq(
                0,
                runProgram(getPython3Binary(), "jstests/ssl_linear/windows_castore_cleanup.py"),
            );

            // SChannel only trusts roots in LocalMachine, so add the trusted CA to Root.
            assert.eq(0, runProgram("certutil.exe", "-addstore", "-f", "Root", TRUSTED_CA_CERT));

            // Import the expired certificate first to bias CertFindCertificateInStore toward
            // returning it before the valid one, reproducing the original failure on unfixed code.
            // We only need the cert to be visible in the store; key accessibility does not matter
            // because the selector loop skips expired certs before it ever tries to acquire the key.
            //
            // Use certutil -addstore (cert-only import from PEM) rather than -importpfx: importing
            // directly from the PEM avoids PKCS#12 format-compatibility issues — Python-generated
            // PFX files have been observed to fail silently on certain Windows versions, leaving the
            // store empty. trusted-server-expired.pem exists in every build (regular and burn-in)
            // because it predates the pkcs12 addition, so no fallback path is needed.
            assert.eq(
                0,
                runProgram(
                    "certutil.exe",
                    "-addstore",
                    "-f",
                    "My",
                    getX509Path("trusted-server-expired.pem"),
                ),
            );

            // Import the valid cert via certutil -mergepfx so Windows correctly links the
            // private key. CryptAcquireCertificatePrivateKey (checked in the selector loop)
            // requires this linkage; PFX files produced by other tools (e.g. Python's
            // cryptography library) do not establish it reliably on Windows.
            const validPfxDir = MongoRunner.toRealPath("$dataDir\\ssl_cert_selector_expired\\");
            mkdir(validPfxDir);
            const validPfxPath = validPfxDir + "trusted-server.pfx";
            assert.eq(
                0,
                runProgram(
                    "certutil.exe",
                    "-mergepfx",
                    "-f",
                    "-p",
                    "qwerty,qwerty",
                    getX509Path("trusted-server.pem"),
                    validPfxPath,
                ),
            );
            assert.eq(
                0,
                runProgram("certutil.exe", "-importpfx", "-f", "-p", "qwerty", validPfxPath),
            );
        });

        after(function () {
            // Remove the trusted CA and both server certs (they share a CN) from the store.
            const trustedCaThumbprint = cat(getX509Path("trusted-ca.pem.digest.sha1"));
            runProgram("certutil.exe", "-delstore", "-f", "Root", trustedCaThumbprint);
            runProgram(getPython3Binary(), "jstests/ssl_linear/windows_castore_cleanup.py");

            // Release the lock last, after all our certs are gone, so the next waiting copy
            // starts its own cleanup with nothing of ours left to race against.
            runProgram(
                getPython3Binary(),
                "jstests/ssl_linear/windows_castore_lock.py",
                "release",
                LOCK_TOKEN,
            );
        });

        it("starts mongod and serves TLS using the valid certificate", function () {
            const conn = MongoRunner.runMongod({
                tlsMode: "requireTLS",
                tlsCertificateSelector: `subject=${SHARED_SUBJECT}`,
                tlsAllowInvalidHostnames: "",
                tlsAllowConnectionsWithoutCertificates: "",
                waitForConnect: true,
                setParameter: {tlsUseSystemCA: true},
            });

            // If the expired certificate had been selected, mongod would have fataled during
            // startup, so a running server already proves the valid duplicate was chosen.
            assert.neq(null, conn, "mongod failed to start with the subject= selector");
            // Match the LOGV2 "id" field specifically (with a trailing word boundary) rather than
            // searching for the bare ID as a substring: EXPIRED_CERT_FATAL_ID's digits can
            // otherwise coincidentally appear inside unrelated large numbers in the log output
            // (e.g. a WiredTiger ts_usec value), causing a flaky false failure.
            assert.eq(
                -1,
                rawMongoProgramOutput(".*").search(new RegExp(`"id":${EXPIRED_CERT_FATAL_ID}\\b`)),
                "mongod reported the selected certificate as expired",
            );

            // Confirm the certificate is actually usable for a TLS handshake.
            assert.eq(
                0,
                runMongoProgram(
                    "mongo",
                    "--tls",
                    "--tlsAllowInvalidHostnames",
                    "--tlsCAFile",
                    TRUSTED_CA_CERT,
                    "--tlsCertificateKeyFile",
                    TRUSTED_CLIENT_CERT,
                    "--port",
                    conn.port,
                    "--eval",
                    "assert.commandWorked(db.adminCommand({ping: 1}));",
                ),
                "client failed to establish a TLS connection to the selected certificate",
            );

            MongoRunner.stopMongod(conn);
        });
    });
});
