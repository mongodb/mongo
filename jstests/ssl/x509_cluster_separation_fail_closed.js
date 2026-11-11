/**
 *
 * Tests that when a certificate qualifies as a cluster member (via the clusterMembership extension)
 * AND an $external user with the same subject DN exists, enforceUserClusterSeparation rejects the
 * connection (see jstests/ssl/cluster_member.js), even if there are failures during user lookup.
 *
 * Any lookup failure that is not "user not found" (e.g. a transient failure induced via maxTimeMS
 * while the user cache is cold) should fail closed, and not falsely report that the user doesn't exist.
 *
 * @tags: [requires_fcv_70]
 */

import {determineSSLProvider} from "jstests/ssl/libs/ssl_helpers.js";

if (determineSSLProvider() !== "openssl") {
    jsTest.log("Test requires openssl based TLS support");
    quit();
}

const FOO_MEMBER_ALT = getX509Path("cluster-member-foo-alt-rdn.pem");
const FOO_MEMBER_ALT_RDN = "CN=Doer,OU=Business,O=Company,L=Fakesville,ST=Example,C=ZZ";

const opts = {
    auth: "",
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: getX509Path("cluster-member-foo.pem"),
    tlsCAFile: getX509Path("ca.pem"),
    clusterAuthMode: "x509",
    tlsClusterAuthX509ExtensionValue: "foo",
    setParameter: {
        enforceUserClusterSeparation: "true",
    },
};

const mongod = MongoRunner.runMongod(opts);

const admin = mongod.getDB("admin");
const ext = mongod.getDB("$external");

// Ensure no localhost auth bypass available.
assert.commandWorked(admin.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
assert(admin.auth("admin", "admin"));

function runAuthSubshell(port, cert, ca, maxTimeMS) {
    function doAuth(maxTimeMS) {
        const external = db.getSiblingDB("$external");
        const authCmd = {authenticate: 1, mechanism: "MONGODB-X509"};
        if (maxTimeMS !== 0) {
            authCmd.maxTimeMS = maxTimeMS;
        }
        const authRes = external.runCommand(authCmd);
        const status = external.adminCommand({connectionStatus: 1});
        const users = (status.authInfo && status.authInfo.authenticatedUsers) || [];
        const isSystem = users.some((u) => u.user === "__system");
        print("AUTH_OK:" + (authRes.ok ? "true" : "false"));
        print("IS_SYSTEM:" + (isSystem ? "true" : "false"));
        print("AUTH_CODE:" + (authRes.code !== undefined ? authRes.code : ""));
        print("AUTH_ERR:" + (authRes.errmsg || ""));
    }
    clearRawMongoProgramOutput();
    runMongoProgram(
        "mongo",
        "--host",
        "localhost",
        "--port",
        port,
        "--tls",
        "--tlsCAFile",
        ca,
        "--tlsCertificateKeyFile",
        cert,
        "--eval",
        `(${doAuth.toString()})(${maxTimeMS});`,
    );
    const out = rawMongoProgramOutput(".*");
    const grab = (marker) => {
        const line = out.split("\n").find((l) => l.includes(marker));
        assert(line, `subshell did not print ${marker}`, {out});
        return line.substring(line.indexOf(marker) + marker.length).trim();
    };
    const codeStr = grab("AUTH_CODE:");
    return {
        authOk: grab("AUTH_OK:") === "true",
        isSystem: grab("IS_SYSTEM:") === "true",
        code: codeStr === "" ? undefined : parseInt(codeStr, 10),
        errmsg: grab("AUTH_ERR:"),
    };
}

const ca = getX509Path("ca.pem");

// Control: auth as cluster member before adding external user
{
    const res = runAuthSubshell(mongod.port, FOO_MEMBER_ALT, ca, 0);
    jsTest.log.info("Auth result with no external user collision", {res});
    assert(res.authOk, "no collision: auth unexpectedly failed", {res});
    assert(res.isSystem, "no collision: __system unexpectedly not granted", {res});
}

// Create a $external user whose name collides with the alt-RDN cluster-member certificate. With
// enforceUserClusterSeparation, presenting that cert must NOT grant __system.
assert.commandWorked(
    ext.runCommand({
        createUser: FOO_MEMBER_ALT_RDN,
        roles: [{db: "admin", role: "readAnyDatabase"}],
    }),
);

// Control (no induced failure, auth should reject):
{
    const res = runAuthSubshell(mongod.port, FOO_MEMBER_ALT, ca, 0);
    jsTest.log.info("Baseline auth result", {res});
    assert(!res.authOk, "baseline: auth unexpectedly succeeded despite user collision", {res});
    assert(!res.isSystem, "baseline: __system unexpectedly granted despite user collision", {res});
}

// Induced failure (auth should still fail):
assert.commandWorked(
    admin.runCommand({configureFailPoint: "authUserCacheSleep", mode: "alwaysOn"}),
);
// Invalidate the user cache so the colliding-user lookup is a cold cache miss that blocks
// (and thus is interruptible by maxTimeMS).
assert.commandWorked(admin.runCommand({invalidateUserCache: 1}));

let systemAccess = false;
try {
    const res = runAuthSubshell(mongod.port, FOO_MEMBER_ALT, ca, 300);
    assert(!res.authOk);
    jsTest.log.info("Exploit auth result", {res});
    systemAccess = res.isSystem;
} finally {
    assert.commandWorked(admin.runCommand({configureFailPoint: "authUserCacheSleep", mode: "off"}));
}

assert(
    !systemAccess,
    "A locked-out cluster-member certificate obtained __system despite enforceUserClusterSeparation.",
);

MongoRunner.stopMongod(mongod);
