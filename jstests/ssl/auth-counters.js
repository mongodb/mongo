// Test for auth counters using MONGODB-X509.

const x509 = "MONGODB-X509";
const mongod = MongoRunner.runMongod({
    auth: "",
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: getX509Path("server.pem"),
    tlsCAFile: getX509Path("ca.pem"),
    clusterAuthMode: "x509",
});
const admin = mongod.getDB("admin");
const external = mongod.getDB("$external");

admin.createUser({user: "admin", pwd: "pwd", roles: ["root"]});
assert(admin.auth("admin", "pwd"));

const X509USER = "CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US";
external.createUser({user: X509USER, roles: []});

// This test ignores counters for SCRAM-SHA-*.
// For those, see jstests/auth/auth-counters.js
const expected = assert.commandWorked(admin.runCommand({serverStatus: 1})).security.authentication.mechanisms[x509];
admin.logout();

function asAdmin(cmd, db = admin) {
    // Does not interfere with stats since we only care about X509.
    assert(admin.auth("admin", "pwd"));
    const result = assert.commandWorked(db.runCommand(cmd));
    admin.logout();
    return result;
}

function assertStats() {
    const mechStats = asAdmin({serverStatus: 1}).security.authentication.mechanisms[x509];

    try {
        assert.eq(mechStats.ingress.authenticate.total, expected.ingress.authenticate.total);
        assert.eq(mechStats.ingress.authenticate.successful, expected.ingress.authenticate.successful);
        assert.eq(mechStats.ingress.clusterAuthenticate.total, expected.ingress.clusterAuthenticate.total);
        assert.eq(mechStats.ingress.clusterAuthenticate.successful, expected.ingress.clusterAuthenticate.successful);
    } catch (e) {
        print("mechStats: " + tojson(mechStats));
        print("expected: " + tojson(expected));
        throw e;
    }
}

function assertSuccess(creds) {
    assert.eq(external.auth(creds), true);
    external.logout();
    ++expected.ingress.authenticate.total;
    ++expected.ingress.authenticate.successful;
    assertStats();
}

function assertFailure(creds) {
    assert.eq(external.auth(creds), false);
    ++expected.ingress.authenticate.total;
    assertStats();
}

function assertSuccessInternal() {
    assert.eq(
        runMongoProgram(
            "mongo",
            "--tls",
            "--port",
            mongod.port,
            "--tlsCertificateKeyFile",
            getX509Path("server.pem"),
            "--tlsCAFile",
            getX509Path("ca.pem"),
            "--authenticationDatabase",
            "$external",
            "--authenticationMechanism",
            "MONGODB-X509",
            "--eval",
            ";",
        ),
        0,
    );
    ++expected.ingress.authenticate.total;
    ++expected.ingress.authenticate.successful;
    ++expected.ingress.clusterAuthenticate.total;
    ++expected.ingress.clusterAuthenticate.successful;
    assertStats();
}

// User from certificate should work.
assertSuccess({mechanism: x509});

// Explicitly named user.
assertSuccess({user: X509USER, mechanism: x509});

// Cluster auth counter checks.
// We can't test failures with the __system user without the handshake failing,
// which won't increment the counters.
assertSuccessInternal();

// Fails once the user no longer exists.
asAdmin({dropUser: X509USER}, external);
assertFailure({mechanism: x509});

const finalStats = asAdmin({serverStatus: 1}).security.authentication.mechanisms;
MongoRunner.stopMongod(mongod);
printjson(finalStats);
