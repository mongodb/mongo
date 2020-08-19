// Test for auth counters in serverStatus.

(function() {
'use strict';

const mongod = MongoRunner.runMongod({auth: ''});
const admin = mongod.getDB('admin');
const test = mongod.getDB('test');

admin.createUser({user: 'admin', pwd: 'pwd', roles: ['root'], mechanisms: ['SCRAM-SHA-256']});
admin.auth('admin', 'pwd');

test.createUser({user: 'user1', pwd: 'pwd', roles: [], mechanisms: ['SCRAM-SHA-1']});
test.createUser({user: 'user256', pwd: 'pwd', roles: [], mechanisms: ['SCRAM-SHA-256']});
test.createUser(
    {user: 'user', pwd: 'pwd', roles: [], mechanisms: ['SCRAM-SHA-1', 'SCRAM-SHA-256']});

// admin.auth() above provides an initial count for SCRAM-SHA-256
const expected = {
    'SCRAM-SHA-256': {
        received: 1,
        successful: 1,
    },
};

function assertStats() {
    const mechStats = assert.commandWorked(admin.runCommand({serverStatus: 1}))
                          .security.authentication.mechanisms;
    Object.keys(expected).forEach(function(mech) {
        try {
            assert.eq(mechStats[mech].authenticate.received, expected[mech].received);
            assert.eq(mechStats[mech].authenticate.successful, expected[mech].successful);
        } catch (e) {
            print("Mechanism: " + mech);
            print("mechStats: " + tojson(mechStats));
            print("expected: " + tojson(expected));
            throw e;
        }
    });
}

function assertSuccess(creds, mech) {
    if (expected[mech] === undefined) {
        expected[mech] = {received: 0, successful: 0};
    }
    assert.eq(test.auth(creds), true);
    test.logout();
    ++expected[mech].received;
    ++expected[mech].successful;
    assertStats();
}

function assertFailure(creds, mech) {
    if (expected[mech] === undefined) {
        expected[mech] = {received: 0, successful: 0};
    }
    assert.eq(test.auth(creds), false);
    ++expected[mech].received;
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

const finalStats =
    assert.commandWorked(admin.runCommand({serverStatus: 1})).security.authentication.mechanisms;
MongoRunner.stopMongod(mongod);

printjson(finalStats);
})();
