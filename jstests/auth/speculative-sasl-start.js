// Test for speculativeSaslStart during isMaster.

const keyFile = "jstests/libs/key1";
const mongod = MongoRunner.runMongod({auth: "", keyFile: keyFile});
const admin = mongod.getDB("admin");

admin.createUser({user: "admin", pwd: "pwd", roles: ["root"], mechanisms: ["SCRAM-SHA-1", "SCRAM-SHA-256"]});
admin.auth("admin", "pwd");

function test(uri, succeed) {
    const shell = runMongoProgram("mongo", uri, "--eval", ";");

    if (succeed) {
        assert.eq(0, shell);
    } else {
        assert.neq(0, shell);
    }
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

// No "intra-cluster" auth attempts yet.
assertStats(function (mechStats) {
    Object.keys(mechStats).forEach(function (mech) {
        const stats = mechStats[mech].clusterAuthenticate;
        assert.eq(stats.received, 0);
        assert.eq(stats.successful, 0);
    });
});

function expectN(mechStats, mech, N1, M1, N2 = 0, M2 = 0) {
    const specStats = mechStats[mech].speculativeAuthenticate;
    const clusterStats = mechStats[mech].clusterAuthenticate;
    assert.eq(N1, specStats.received);
    assert.eq(M1, specStats.successful);
    assert.eq(N2, clusterStats.received);
    assert.eq(M2, clusterStats.successful);
}

function assertSpecLog(severity, expectedAttrs, expectedCount) {
    assert(checkLog.checkContainsWithCountJson(admin, 5286307, expectedAttrs, expectedCount, severity));
}

const baseOKURI = "mongodb://admin:pwd@localhost:" + mongod.port + "/admin";

// Speculate SCRAM-SHA-1
test(baseOKURI + "?authMechanism=SCRAM-SHA-1", true);
assertStats((s) => expectN(s, "SCRAM-SHA-1", 1, 1));
assertStats((s) => expectN(s, "SCRAM-SHA-256", 0, 0));

// Speculate SCRAM-SHA-256
test(baseOKURI + "?authMechanism=SCRAM-SHA-256", true);
assertStats((s) => expectN(s, "SCRAM-SHA-1", 1, 1));
assertStats((s) => expectN(s, "SCRAM-SHA-256", 1, 1));

// Fallback should speculate SCRAM-SHA-256
test(baseOKURI, true);
assertStats((s) => expectN(s, "SCRAM-SHA-1", 1, 1));
assertStats((s) => expectN(s, "SCRAM-SHA-256", 2, 2));

const baseFAILURI = "mongodb://admin:haxx@localhost:" + mongod.port + "/admin";
// Client is impossible to match using checklog.
const speculativeSHA1AuthFailedAttrs = {
    "mechanism": "SCRAM-SHA-1",
    "isSpeculative": true,
    "user": "admin",
    "db": "admin",
    "error": "AuthenticationFailed: SCRAM authentication failed, storedKey mismatch",
};
const sha1AuthFailedAttrs = {
    "mechanism": "SCRAM-SHA-1",
    "isSpeculative": false,
    "user": "admin",
    "db": "admin",
    "error": "AuthenticationFailed: SCRAM authentication failed, storedKey mismatch",
};
const speculativeSHA256AuthFailedAttrs = {
    "mechanism": "SCRAM-SHA-256",
    "isSpeculative": true,
    "user": "admin",
    "db": "admin",
    "error": "AuthenticationFailed: SCRAM authentication failed, storedKey mismatch",
};
const sha256AuthFailedAttrs = {
    "mechanism": "SCRAM-SHA-256",
    "isSpeculative": false,
    "user": "admin",
    "db": "admin",
    "error": "AuthenticationFailed: SCRAM authentication failed, storedKey mismatch",
};
const speculativeSHA256MechUnavailableAttrs = {
    "mechanism": "SCRAM-SHA-256",
    "isSpeculative": true,
    "user": "admin",
    "db": "admin",
    "error":
        "MechanismUnavailable: Unable to use SCRAM-SHA-256 based authentication for user without any SCRAM-SHA-256 credentials registered",
};
const sha256MechUnavailableAttrs = {
    "mechanism": "SCRAM-SHA-256",
    "isSpeculative": false,
    "user": "admin",
    "db": "admin",
    "error":
        "MechanismUnavailable: Unable to use SCRAM-SHA-256 based authentication for user without any SCRAM-SHA-256 credentials registered",
};
const speculativeClusterAuthFailedAttrs = {
    "mechanism": "SCRAM-SHA-256",
    "isSpeculative": true,
    "user": "__system",
    "db": "local",
    "error": "AuthenticationFailed: SCRAM authentication failed, storedKey mismatch",
};
const clusterAuthFailedAttrs = {
    "mechanism": "SCRAM-SHA-256",
    "isSpeculative": false,
    "user": "__system",
    "db": "local",
    "error": "AuthenticationFailed: SCRAM authentication failed, storedKey mismatch",
};
admin.setLogLevel(5);
// Invalid password should never connect regardless of speculative auth.
test(baseFAILURI + "?authMechanism=SCRAM-SHA-1", false);
assertStats((s) => expectN(s, "SCRAM-SHA-1", 2, 1));
assertStats((s) => expectN(s, "SCRAM-SHA-256", 2, 2));
assertSpecLog("I", speculativeSHA1AuthFailedAttrs, 1);
assertSpecLog("I", sha1AuthFailedAttrs, 1);

test(baseFAILURI + "?authMechanism=SCRAM-SHA-256", false);
assertStats((s) => expectN(s, "SCRAM-SHA-1", 2, 1));
assertStats((s) => expectN(s, "SCRAM-SHA-256", 3, 2));
assertSpecLog("I", speculativeSHA256AuthFailedAttrs, 1);
assertSpecLog("I", sha256AuthFailedAttrs, 1);

test(baseFAILURI, false);
assertStats((s) => expectN(s, "SCRAM-SHA-1", 2, 1));
assertStats((s) => expectN(s, "SCRAM-SHA-256", 4, 2));
assertSpecLog("I", speculativeSHA256AuthFailedAttrs, 2);
assertSpecLog("I", sha256AuthFailedAttrs, 2);

// Update admin use to only allow SCRAM-SHA-1

admin.updateUser("admin", {mechanisms: ["SCRAM-SHA-1"]});

// Fallback (SCRAM-SHA-256) should fail to speculate.
test(baseOKURI, true);
assertStats((s) => expectN(s, "SCRAM-SHA-1", 2, 1));
assertStats((s) => expectN(s, "SCRAM-SHA-256", 5, 2));
assertSpecLog("I", speculativeSHA256MechUnavailableAttrs, 1);

// Explicit SCRAM-SHA-1 should successfully speculate.
test(baseOKURI + "?authMechanism=SCRAM-SHA-1", true);
assertStats((s) => expectN(s, "SCRAM-SHA-1", 3, 2));
assertStats((s) => expectN(s, "SCRAM-SHA-256", 5, 2));

// Explicit SCRAM-SHA-256 should fail to speculate or connect at all.
test(baseOKURI + "?authMechanism=SCRAM-SHA-256", false);
assertStats((s) => expectN(s, "SCRAM-SHA-1", 3, 2));
assertStats((s) => expectN(s, "SCRAM-SHA-256", 6, 2));
assertSpecLog("I", speculativeSHA256MechUnavailableAttrs, 2);
assertSpecLog("I", sha256MechUnavailableAttrs, 1);

// Test that a user not in the admin DB can speculate
mongod.getDB("test").createUser({user: "alice", pwd: "secret", roles: []});
test("mongodb://alice:secret@localhost:" + mongod.port + "/test", true);
assertStats((s) => expectN(s, "SCRAM-SHA-256", 7, 3));

// Test "intra-cluster" speculative authentication.
const systemPass = cat(keyFile).replace(/\s/g, "");
test(
    "mongodb://__system:" + systemPass + "@localhost:" + mongod.port + "/admin" + "?authMechanism=SCRAM-SHA-256",
    true,
);
assertStats((s) => expectN(s, "SCRAM-SHA-256", 8, 4, 1, 1));

test("mongodb://__system:hunter2@localhost:" + mongod.port + "/admin" + "?authMechanism=SCRAM-SHA-256", false);
assertStats((s) => expectN(s, "SCRAM-SHA-256", 9, 4, 3, 1));
assertSpecLog("I", speculativeClusterAuthFailedAttrs, 1);
assertSpecLog("I", clusterAuthFailedAttrs, 1);

admin.setLogLevel(0);

MongoRunner.stopMongod(mongod);
