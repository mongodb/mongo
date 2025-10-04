// Test for speculativeAuthenticate during isMaster.

const mongod = MongoRunner.runMongod({
    auth: "",
    tlsMode: "requireTLS",
    tlsCertificateKeyFile: "jstests/libs/server.pem",
    tlsCAFile: "jstests/libs/ca.pem",
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
        "jstests/libs/ca.pem",
        "--tlsCertificateKeyFile",
        "jstests/libs/client.pem",
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
        "jstests/libs/ca.pem",
        "--tlsCertificateKeyFile",
        "jstests/libs/server.pem",
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
        const stats = mechStats[mech].speculativeAuthenticate;
        assert.eq(stats.received, 0);
        assert.eq(stats.successful, 0);
    });
});

// Connect with speculation and have 1/1 result.
const baseURI = "mongodb://localhost:" + mongod.port + "/admin";
test(baseURI + "?authMechanism=MONGODB-X509");
assertStats(function (mechStats) {
    const stats = mechStats["MONGODB-X509"].speculativeAuthenticate;
    assert.eq(stats.received, 1);
    assert.eq(stats.successful, 1);
});

// Connect without speculation and still have 1/1 result.
test(baseURI);
assertStats(function (mechStats) {
    const stats = mechStats["MONGODB-X509"].speculativeAuthenticate;
    assert.eq(stats.received, 1);
    assert.eq(stats.successful, 1);
});

// We haven't done any cluster auth yet, so clusterAuthenticate counts should be 0
assertStats(function (mechStats) {
    const stats = mechStats["MONGODB-X509"].clusterAuthenticate;
    assert.eq(stats.received, 0);
    assert.eq(stats.successful, 0);
});

// Connect intra-cluster with speculation.
testInternal(baseURI + "?authMechanism=MONGODB-X509");
assertStats(function (mechStats) {
    const specStats = mechStats["MONGODB-X509"].speculativeAuthenticate;
    const clusterStats = mechStats["MONGODB-X509"].clusterAuthenticate;
    assert.eq(specStats.received, 2);
    assert.eq(specStats.successful, 2);
    assert.eq(clusterStats.received, 1);
    assert.eq(clusterStats.successful, 1);
});

MongoRunner.stopMongod(mongod);
