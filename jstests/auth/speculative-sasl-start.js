// Test for speculativeSaslStart during isMaster.

(function() {
'use strict';

const mongod = MongoRunner.runMongod({auth: ''});
const admin = mongod.getDB('admin');

admin.createUser(
    {user: 'admin', pwd: 'pwd', roles: ['root'], mechanisms: ['SCRAM-SHA-1', 'SCRAM-SHA-256']});
admin.auth('admin', 'pwd');

function test(uri, succeed) {
    const shell = runMongoProgram('mongo', uri, '--eval', ';');

    if (succeed) {
        assert.eq(0, shell);
    } else {
        assert.neq(0, shell);
    }
}

function assertStats(cb) {
    const mechStats = assert.commandWorked(admin.runCommand({serverStatus: 1}))
                          .security.authentication.mechanisms;
    cb(mechStats);
}

// No speculative auth attempts yet.
assertStats(function(mechStats) {
    Object.keys(mechStats).forEach(function(mech) {
        const stats = mechStats[mech].speculativeAuthenticate;
        assert.eq(stats.received, 0);
        assert.eq(stats.successful, 0);
    });
});

function expectN(mechStats, mech, N, M) {
    const stats = mechStats[mech].speculativeAuthenticate;
    assert.eq(N, stats.received);
    assert.eq(M, stats.successful);
}

function assertSpecLog(severity, expectedAttrs, expectedCount) {
    assert(
        checkLog.checkContainsWithCountJson(admin, 20249, expectedAttrs, expectedCount, severity));
}

const baseOKURI = 'mongodb://admin:pwd@localhost:' + mongod.port + '/admin';

// Speculate SCRAM-SHA-1
test(baseOKURI + '?authMechanism=SCRAM-SHA-1', true);
assertStats((s) => expectN(s, 'SCRAM-SHA-1', 1, 1));
assertStats((s) => expectN(s, 'SCRAM-SHA-256', 0, 0));

// Speculate SCRAM-SHA-256
test(baseOKURI + '?authMechanism=SCRAM-SHA-256', true);
assertStats((s) => expectN(s, 'SCRAM-SHA-1', 1, 1));
assertStats((s) => expectN(s, 'SCRAM-SHA-256', 1, 1));

// Fallback should speculate SCRAM-SHA-256
test(baseOKURI, true);
assertStats((s) => expectN(s, 'SCRAM-SHA-1', 1, 1));
assertStats((s) => expectN(s, 'SCRAM-SHA-256', 2, 2));

const baseFAILURI = 'mongodb://admin:haxx@localhost:' + mongod.port + '/admin';
// Client is impossible to match using checklog.
const speculativeSHA1AuthFailedAttrs = {
    "mechanism": "SCRAM-SHA-1",
    "speculative": true,
    "principalName": "admin",
    "authenticationDatabase": "admin",
    "error": "AuthenticationFailed: SCRAM authentication failed, storedKey mismatch"
};
const sha1AuthFailedAttrs = {
    "mechanism": "SCRAM-SHA-1",
    "speculative": false,
    "principalName": "admin",
    "authenticationDatabase": "admin",
    "error": "AuthenticationFailed: SCRAM authentication failed, storedKey mismatch"
};
const speculativeSHA256AuthFailedAttrs = {
    "mechanism": "SCRAM-SHA-256",
    "speculative": true,
    "principalName": "admin",
    "authenticationDatabase": "admin",
    "error": "AuthenticationFailed: SCRAM authentication failed, storedKey mismatch"
};
const sha256AuthFailedAttrs = {
    "mechanism": "SCRAM-SHA-256",
    "speculative": false,
    "principalName": "admin",
    "authenticationDatabase": "admin",
    "error": "AuthenticationFailed: SCRAM authentication failed, storedKey mismatch"
};
const speculativeSHA256MechUnavailableAttrs = {
    "mechanism": "SCRAM-SHA-256",
    "speculative": true,
    "principalName": "admin",
    "authenticationDatabase": "admin",
    "error":
        "MechanismUnavailable: Unable to use SCRAM-SHA-256 based authentication for user without any SCRAM-SHA-256 credentials registered"
};
const sha256MechUnavailableAttrs = {
    "mechanism": "SCRAM-SHA-256",
    "speculative": false,
    "principalName": "admin",
    "authenticationDatabase": "admin",
    "error":
        "MechanismUnavailable: Unable to use SCRAM-SHA-256 based authentication for user without any SCRAM-SHA-256 credentials registered"
};
admin.setLogLevel(5);
// Invalid password should never connect regardless of speculative auth.
test(baseFAILURI + '?authMechanism=SCRAM-SHA-1', false);
assertStats((s) => expectN(s, 'SCRAM-SHA-1', 2, 1));
assertStats((s) => expectN(s, 'SCRAM-SHA-256', 2, 2));
assertSpecLog("I", speculativeSHA1AuthFailedAttrs, 1);
assertSpecLog("I", sha1AuthFailedAttrs, 1);

test(baseFAILURI + '?authMechanism=SCRAM-SHA-256', false);
assertStats((s) => expectN(s, 'SCRAM-SHA-1', 2, 1));
assertStats((s) => expectN(s, 'SCRAM-SHA-256', 3, 2));
assertSpecLog("I", speculativeSHA256AuthFailedAttrs, 1);
assertSpecLog("I", sha256AuthFailedAttrs, 1);

test(baseFAILURI, false);
assertStats((s) => expectN(s, 'SCRAM-SHA-1', 2, 1));
assertStats((s) => expectN(s, 'SCRAM-SHA-256', 4, 2));
assertSpecLog("I", speculativeSHA256AuthFailedAttrs, 2);
assertSpecLog("I", sha256AuthFailedAttrs, 2);

// Update admin use to only allow SCRAM-SHA-1

admin.updateUser('admin', {mechanisms: ['SCRAM-SHA-1']});

// Fallback (SCRAM-SHA-256) should fail to speculate.
test(baseOKURI, true);
assertStats((s) => expectN(s, 'SCRAM-SHA-1', 2, 1));
assertStats((s) => expectN(s, 'SCRAM-SHA-256', 5, 2));
assertSpecLog("D5", speculativeSHA256MechUnavailableAttrs, 1);

// Explicit SCRAM-SHA-1 should successfully speculate.
test(baseOKURI + '?authMechanism=SCRAM-SHA-1', true);
assertStats((s) => expectN(s, 'SCRAM-SHA-1', 3, 2));
assertStats((s) => expectN(s, 'SCRAM-SHA-256', 5, 2));

// Explicit SCRAM-SHA-256 should fail to speculate or connect at all.
test(baseOKURI + '?authMechanism=SCRAM-SHA-256', false);
assertStats((s) => expectN(s, 'SCRAM-SHA-1', 3, 2));
assertStats((s) => expectN(s, 'SCRAM-SHA-256', 6, 2));
assertSpecLog("D5", speculativeSHA256MechUnavailableAttrs, 2);
assertSpecLog("I", sha256MechUnavailableAttrs, 1);

// Test that a user not in the admin DB can speculate
mongod.getDB('test').createUser({user: 'alice', pwd: 'secret', roles: []});
test('mongodb://alice:secret@localhost:' + mongod.port + '/test', true);
assertStats((s) => expectN(s, 'SCRAM-SHA-256', 7, 3));

admin.setLogLevel(0);

MongoRunner.stopMongod(mongod);
})();
