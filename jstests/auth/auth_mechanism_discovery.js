// Tests that a client will auto-discover a user's supported SASL mechanisms during auth().
(function() {
    "use strict";

    function runTest(conn) {
        const admin = conn.getDB("admin");
        const test = conn.getDB("test");

        admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
        assert(admin.auth('admin', 'pass'));

        // Enable SCRAM-SHA-256.
        assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: "4.0"}));

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
    }

    // Test standalone.
    const m = MongoRunner.runMongod({auth: ""});
    runTest(m);
    MongoRunner.stopMongod(m);

    // Test sharded.
    // TODO: Remove 'shardAsReplicaSet: false' when SERVER-32672 is fixed.
    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 1,
        other: {keyFile: 'jstests/libs/key1', shardAsReplicaSet: false}
    });
    runTest(st.s0);
    st.stop();
})();
