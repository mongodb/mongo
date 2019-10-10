// Tests that a client will auto-discover a user's supported SASL mechanisms during auth().
// @tags: [requires_sharding]
(function() {
"use strict";

function runTest(conn) {
    const admin = conn.getDB("admin");
    const test = conn.getDB("test");

    admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
    assert(admin.auth('admin', 'pass'));

    // Verify user mechanism discovery.
    function checkUser(username, mechanism) {
        var createUser = {createUser: username, pwd: 'pwd', roles: []};
        if (mechanism !== undefined) {
            createUser.mechanisms = [mechanism];
        } else {
            // Create both variants, expect to prefer 256.
            mechanism = 'SCRAM-SHA-256';
        }
        assert.commandWorked(test.runCommand(createUser));
        assert.eq(test._getDefaultAuthenticationMechanism(username, test.getName()), mechanism);
        assert(test.auth(username, 'pwd'));
        test.logout();
    }
    checkUser('userSha1', 'SCRAM-SHA-1');
    checkUser('userSha256', 'SCRAM-SHA-256');
    checkUser('userAll');

    // Verify override of mechanism discovery.
    // Depends on 'userAll' user created above.
    assert.eq(test._getDefaultAuthenticationMechanism('userAll', test.getName()), 'SCRAM-SHA-256');
    test._defaultAuthenticationMechanism = 'SCRAM-SHA-1';
    assert.eq(test._getDefaultAuthenticationMechanism('userAll', test.getName()), 'SCRAM-SHA-1');
    test._defaultAuthenticationMechanism = 'NO-SUCH-MECHANISM';
    assert.eq(test._getDefaultAuthenticationMechanism('userAll', test.getName()), 'SCRAM-SHA-256');
}

// Test standalone.
const m = MongoRunner.runMongod({auth: ""});
runTest(m);
MongoRunner.stopMongod(m);

// Test sharded.
const st =
    new ShardingTest({shards: 1, mongos: 1, config: 1, other: {keyFile: 'jstests/libs/key1'}});
runTest(st.s0);
st.stop();
})();
