// Test for auth counters in serverStatus.
// @tags: [requires_replication]

(function() {
'use strict';

const keyfile = 'jstests/libs/key1';
const badKeyfile = 'jstests/libs/key2';
let replTest = new ReplSetTest({nodes: 1, keyFile: keyfile, nodeOptions: {auth: ""}});
replTest.startSet();
replTest.initiate();
let primary = replTest.getPrimary();

const admin = primary.getDB('admin');
const test = primary.getDB('test');

admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root'], mechanisms: ['SCRAM-SHA-256']});
admin.auth('admin', 'pwd');

test.createUser({user: 'user1', pwd: 'pwd', roles: [], mechanisms: ['SCRAM-SHA-1']});
test.createUser({user: 'user256', pwd: 'pwd', roles: [], mechanisms: ['SCRAM-SHA-256']});
test.createUser(
    {user: 'user', pwd: 'pwd', roles: [], mechanisms: ['SCRAM-SHA-1', 'SCRAM-SHA-256']});

// Count the number of authentications performed during setup
const expected =
    assert.commandWorked(admin.runCommand({serverStatus: 1})).security.authentication.mechanisms;

function assertStats() {
    const mechStats = assert.commandWorked(admin.runCommand({serverStatus: 1}))
                          .security.authentication.mechanisms;
    Object.keys(expected).forEach(function(mech) {
        try {
            assert.eq(mechStats[mech].authenticate.received, expected[mech].authenticate.received);
            assert.eq(mechStats[mech].authenticate.successful,
                      expected[mech].authenticate.successful);
            assert.eq(mechStats[mech].clusterAuthenticate.received,
                      expected[mech].clusterAuthenticate.received);
            assert.eq(mechStats[mech].clusterAuthenticate.successful,
                      expected[mech].clusterAuthenticate.successful);
        } catch (e) {
            print("Mechanism: " + mech);
            print("mechStats: " + tojson(mechStats));
            print("expected: " + tojson(expected));
            throw e;
        }
    });
}

function assertSuccess(creds, mech, db = test) {
    assert.eq(db.auth(creds), true);
    if (db !== admin) {
        db.logout();
    }
    ++expected[mech].authenticate.received;
    ++expected[mech].authenticate.successful;
    assertStats();
}

function assertFailure(creds, mech, db = test) {
    assert.eq(db.auth(creds), false);
    ++expected[mech].authenticate.received;
    assertStats();
}

function assertSuccessInternal() {
    const mech = "SCRAM-SHA-1";
    // asCluster exiting cleanly indicates successful auth
    assert.eq(authutil.asCluster(replTest.nodes, keyfile, () => true), true);
    ++expected[mech].authenticate.received;
    ++expected[mech].authenticate.successful;
    ++expected[mech].clusterAuthenticate.received;
    ++expected[mech].clusterAuthenticate.successful;
    // we have to re-auth as admin to get stats, which are validated at the end of assertSuccess
    assertSuccess({user: 'admin', pwd: 'pwd'}, 'SCRAM-SHA-256', admin);
}

function assertFailureInternal() {
    const mech = "SCRAM-SHA-1";
    // If asCluster fails, it explodes.
    assert.throws(authutil.asCluster, [replTest.nodes, badKeyfile, () => true]);
    ++expected[mech].authenticate.received;
    ++expected[mech].clusterAuthenticate.received;
    // we have to re-auth as admin to get stats, which are validated at the end of assertSuccess
    assertSuccess({user: 'admin', pwd: 'pwd'}, 'SCRAM-SHA-256', admin);
    assertStats();
}

// Initial condition, one auth by admin during user setups above.
// Using negotiated SCRAM-SHA-256 only.
assertStats();

// user1 should negotiate and succeed at SHA1
assertSuccess({user: 'user1', pwd: 'pwd'}, 'SCRAM-SHA-1');

// user and user256 should both negotiate and success at SHA256
assertSuccess({user: 'user256', pwd: 'pwd'}, 'SCRAM-SHA-256');
assertSuccess({user: 'user', pwd: 'pwd'}, 'SCRAM-SHA-256');

// user, user1, and user256 as above, but explicitly asking for mechanisms.
assertSuccess({user: 'user1', pwd: 'pwd', mechanism: 'SCRAM-SHA-1'}, 'SCRAM-SHA-1');
assertSuccess({user: 'user256', pwd: 'pwd', mechanism: 'SCRAM-SHA-256'}, 'SCRAM-SHA-256');
assertSuccess({user: 'user', pwd: 'pwd', mechanism: 'SCRAM-SHA-1'}, 'SCRAM-SHA-1');
assertSuccess({user: 'user', pwd: 'pwd', mechanism: 'SCRAM-SHA-256'}, 'SCRAM-SHA-256');

// Incorrect password.
assertFailure({user: 'user1', pwd: 'haxx'}, 'SCRAM-SHA-1');
assertFailure({user: 'user256', pwd: 'haxx'}, 'SCRAM-SHA-256');
assertFailure({user: 'user', pwd: 'haxx'}, 'SCRAM-SHA-256');
assertFailure({user: 'user', pwd: 'haxx', mechanism: 'SCRAM-SHA-1'}, 'SCRAM-SHA-1');

// Incorrect mechanism.
assertFailure({user: 'user1', pwd: 'pwd', mechanism: 'SCRAM-SHA-256'}, 'SCRAM-SHA-256');
assertFailure({user: 'user256', pwd: 'pwd', mechanism: 'SCRAM-SHA-1'}, 'SCRAM-SHA-1');

// Cluster auth counter checks.
assertSuccessInternal();
assertFailureInternal();

// Need to auth as admin one more time to get final stats.
admin.auth('admin', 'pwd');

const finalStats =
    assert.commandWorked(admin.runCommand({serverStatus: 1})).security.authentication.mechanisms;
replTest.stopSet();

printjson(finalStats);
})();
