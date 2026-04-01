// Test for speculativeAuthenticate during isMaster.

const mongod = MongoRunner.runMongod({
    auth: "",
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: getX509Path("server.pem"),
    tlsCAFile: getX509Path("ca.pem"),
    clusterAuthMode: "x509",
});
const admin = mongod.getDB("admin");
const external = mongod.getDB("$external");

admin.createUser({user: "admin", pwd: "pwd", roles: ["root"], mechanisms: ["SCRAM-SHA-1", "SCRAM-SHA-256"]});
admin.auth("admin", "pwd");

const X509USER = "CN=client,OU=KernelUser,O=MongoDB,L=New York City,ST=New York,C=US";
external.createUser({user: X509USER, roles: [{role: "root", db: "admin"}]});

function test(uri) {
    const x509 = runMongoProgram(
        "mongo",
        "--tls",
        "--tlsCAFile",
        getX509Path("ca.pem"),
        "--tlsCertificateKeyFile",
        getX509Path("client.pem"),
        uri,
        "--eval",
        ";",
    );
    assert.eq(0, x509);
}

function testInternal(uri) {
    const x509 = runMongoProgram(
        "mongo",
        "--tls",
        "--tlsCAFile",
        getX509Path("ca.pem"),
        "--tlsCertificateKeyFile",
        getX509Path("server.pem"),
        uri,
        "--eval",
        ";",
    );
    assert.eq(0, x509);
}

function assertStats(cb) {
    const mechStats = assert.commandWorked(admin.runCommand({serverStatus: 1})).security.authentication.mechanisms;
    cb(mechStats);
}

// No speculative auth attempts yet.
assertStats(function (mechStats) {
    Object.keys(mechStats).forEach(function (mech) {
        if (mech.hasOwnProperty("ingress")) {
            const stats = mechStats[mech].ingress.speculativeAuthenticate;
            assert.eq(stats.total, 0);
            assert.eq(stats.successful, 0);
        }
    });
});

// Connect with speculation and have 1/1 result.
const baseURI = "mongodb://localhost:" + mongod.port + "/admin";
test(baseURI + "?authMechanism=MONGODB-X509");
assertStats(function (mechStats) {
    const stats = mechStats["MONGODB-X509"].ingress.speculativeAuthenticate;
    assert.eq(stats.total, 1);
    assert.eq(stats.successful, 1);
});

// Connect without speculation and still have 1/1 result.
test(baseURI);
assertStats(function (mechStats) {
    const stats = mechStats["MONGODB-X509"].ingress.speculativeAuthenticate;
    assert.eq(stats.total, 1);
    assert.eq(stats.successful, 1);
});

// We haven't done any cluster auth yet, so clusterAuthenticate counts should be 0
assertStats(function (mechStats) {
    const stats = mechStats["MONGODB-X509"].ingress.clusterAuthenticate;
    assert.eq(stats.total, 0);
    assert.eq(stats.successful, 0);
});

// Connect intra-cluster with speculation.
testInternal(baseURI + "?authMechanism=MONGODB-X509");
assertStats(function (mechStats) {
    const specStats = mechStats["MONGODB-X509"].ingress.speculativeAuthenticate;
    const clusterStats = mechStats["MONGODB-X509"].ingress.clusterAuthenticate;
    assert.eq(specStats.total, 2);
    assert.eq(specStats.successful, 2);
    assert.eq(clusterStats.total, 1);
    assert.eq(clusterStats.successful, 1);
});

MongoRunner.stopMongod(mongod);
