/**
 * Verifies that the Landlock sandbox actually denies a path the policy did not grant, by pointing
 * one of the server's startup paths at a symbolic link and re-pointing the link after the sandbox
 * has engaged.
 *
 * The policy resolves symbolic links when it derives a rule (see canonicalizePathForLandlock), so
 * the grant lands on the directory the link resolved to at startup. The kernel resolves the link
 * again on every open, and checks the result against the ruleset. A link re-pointed at a directory
 * outside the grant must therefore be denied, even though the link itself, and the file it now
 * names, are unchanged from anything the server was ever configured with.
 *
 * rotateCertificates re-reads the configured TLS certificate/key file after startup, which makes
 * it a way to make a running server open the path again on demand. A rotation that cannot read
 * the file fails and leaves the previous certificate in use.
 *
 * Everything lives under /var/tmp, which the policy does not grant, so neither the original nor
 * the replacement directory is covered by some unrelated rule (as /tmp would be).
 * The node runs "bestEffort"; on a kernel without Landlock the denial step is skipped, since the
 * rotation would legitimately succeed there.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {copyCertificateFile} from "jstests/ssl/libs/ssl_helpers.js";

if (getBuildInfo().buildEnvironment.target_os !== "linux") {
    jsTest.log.info("Landlock is Linux-only; skipping test");
    quit();
}

const kIdRuleApplied = 13118805; // one path rule granted (attr.path is the resolved path)

describe("a startup path that is a symbolic link re-pointed outside the grant", function () {
    const root = `/var/tmp/${jsTestName()}-${UUID().hex()}`;
    // The directory the link resolves to at startup, so the one the policy grants.
    const grantedDir = `${root}/granted`;
    // Never named at startup, so nothing grants it.
    const ungrantedDir = `${root}/ungranted`;
    const link = `${root}/server-link.pem`;

    let conn;
    let sandboxed;

    /** The paths of every filesystem rule the server logged as applied at startup. */
    function grantedRulePaths() {
        return checkLog
            .getGlobalLog(conn)
            .filter((line) => line.includes(`"id":${kIdRuleApplied}`))
            .map((line) => JSON.parse(line).attr.path);
    }

    function assertServesValidCertificate() {
        assert.eq(
            0,
            runMongoProgram(
                "mongo",
                "--host",
                "localhost",
                "--port",
                conn.port,
                "--tls",
                "--tlsCAFile",
                getX509Path("ca.pem"),
                "--eval",
                ";",
            ),
            "could not connect over TLS with certificate validation",
        );
    }

    /** Atomically replaces `link` so that it names `target`. */
    function pointLinkAt(target) {
        assert.eq(0, runNonMongoProgram("ln", "-sfn", target, link), `failed to link ${link}`);
    }

    before(function () {
        mkdir(root);
        mkdir(grantedDir);
        mkdir(ungrantedDir);

        // Identical, valid certificates in each location: a rotation can only fail because the
        // file could not be opened, never because of its contents or its permissions.
        for (const file of [
            `${grantedDir}/server.pem`,
            `${grantedDir}/server2.pem`,
            `${ungrantedDir}/server.pem`,
        ]) {
            copyCertificateFile(getX509Path("server.pem"), file);
        }
        pointLinkAt(`${grantedDir}/server.pem`);

        conn = MongoRunner.runMongod({
            landlockMode: "bestEffort",
            // Not requireTLS: the shell connects without TLS here. The server still loads and
            // rotates the certificate, which is all this test needs of it.
            tlsMode: "allowTLS",
            tlsCertificateKeyFile: link,
            tlsCAFile: getX509Path("ca.pem"),
            tlsAllowConnectionsWithoutCertificates: "",
        });
        assert.neq(null, conn, "mongod failed to start with a symbolic link as its certificate");

        const landlock = assert.commandWorked(conn.adminCommand({serverStatus: 1})).landlock;
        sandboxed = landlock.active;
        jsTest.log.info("Landlock sandbox status on this kernel", {landlock});
    });

    // Guardrail for the premise of this whole test: the denial below proves something only while
    // the built-in policy grants nothing that covers /var/tmp. If a future policy change grants
    // /var/tmp (or an ancestor, up to "/"), the denial step would fail for a confusing reason, so
    // fail here, naming the rule.
    it("is not undermined by a built-in rule covering the scratch directory", function () {
        if (!sandboxed) {
            jsTest.log.info("kernel does not enforce Landlock; nothing to assert");
            return;
        }
        const paths = grantedRulePaths();
        // The rules are read from getLog's in-memory buffer, which only keeps the most recent
        // entries; none found would mean they rotated out, not that nothing was granted.
        assert.gt(paths.length, 0, "no Landlock path rules found in the in-memory log");
        for (const path of paths) {
            const covers = path === "/" || root === path || root.startsWith(path + "/");
            assert(
                !covers,
                `the Landlock policy grants '${path}', which covers the test's scratch ` +
                    `directory ${root}; this test needs a location the policy does not grant`,
                {paths},
            );
        }
    });

    it("rotates the certificate through the link as configured", function () {
        // Also the control for the TLS check used after the denial: it works on this node.
        assertServesValidCertificate();
        assert.commandWorked(conn.adminCommand({rotateCertificates: 1}));
    });

    it("rotates after the link moves within the granted directory", function () {
        // The control for the denial below: re-pointing a link is not by itself what breaks a
        // rotation, only what the link now resolves to.
        pointLinkAt(`${grantedDir}/server2.pem`);
        assert.commandWorked(conn.adminCommand({rotateCertificates: 1}));
    });

    it("fails to rotate after the link moves outside the granted directory", function () {
        if (!sandboxed) {
            jsTest.log.info("kernel does not enforce Landlock; nothing to assert");
            return;
        }
        pointLinkAt(`${ungrantedDir}/server.pem`);
        assert.commandFailed(
            conn.adminCommand({rotateCertificates: 1}),
            `rotation read ${ungrantedDir}/server.pem, which no Landlock rule grants`,
        );

        // The failed rotation must leave the server serving with the certificate it had.
        assertServesValidCertificate();
    });

    it("rotates again once the link returns to the granted directory", function () {
        // Nothing was lost by the denial: the grant is intact and the same link works again.
        pointLinkAt(`${grantedDir}/server.pem`);
        assert.commandWorked(conn.adminCommand({rotateCertificates: 1}));
    });

    after(function () {
        if (conn) {
            MongoRunner.stopMongod(conn);
        }
        // Outside the test's data directory, so resmoke will not clean it up.
        removeFile(root);
    });
});
