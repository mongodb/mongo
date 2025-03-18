// Test that manually inserted impersonation can't escalate privileges.
// @tags: [requires_replication]

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(conn, keyFile = undefined) {
    const admin = conn.getDB('admin');
    admin.createUser({user: 'admin', pwd: 'admin', roles: ['root']});

    function assertError(cmd, msg, code) {
        const errmsg = assert.commandFailedWithCode(admin.runCommand(cmd), code).errmsg;
        assert(errmsg.includes(msg), "Error message is missing '" + msg + "': " + errmsg);
    }

    jsTest.log('Start - Sanity check without impersonation metadata');

    // Localhost authbypass is disabled, and we haven't logged in,
    // so normal auth-required commands should fail.
    assertError(
        {usersInfo: 1}, 'Command usersInfo requires authentication', ErrorCodes.Unauthorized);

    // Hello command requires no auth, so it works fine.
    assert.commandWorked(admin.runCommand({hello: 1}));

    jsTest.log('Negative tests - Add impersonation metadata to hello command');

    // Adding impersonation metadata is forbidden if we're not permitted to use it.
    const kImpersonatedUserHello = {
        hello: 1,
        "$audit": {
            "$impersonatedUser": {user: 'admin', db: 'admin'},
            "$impersonatedRoles": [{role: 'root', db: 'admin'}],
        }
    };
    const kImpersonatedClientHello = {
        hello: 1,
        "$audit": {
            "$impersonatedClient": {hosts: ['172.23.55.11:23890', '192.43.22.3:14089']},
            "$impersonatedRoles": [],
        }
    };
    assertError(kImpersonatedUserHello,
                'Unauthorized use of impersonation metadata',
                ErrorCodes.Unauthorized);
    assertError(kImpersonatedClientHello,
                'Unauthorized use of impersonation metadata',
                ErrorCodes.Unauthorized);

    // Try as admin (root role), should still fail.
    admin.auth('admin', 'admin');
    assertError(kImpersonatedUserHello,
                'Unauthorized use of impersonation metadata',
                ErrorCodes.Unauthorized);
    assertError(kImpersonatedClientHello,
                'Unauthorized use of impersonation metadata',
                ErrorCodes.Unauthorized);
    admin.logout();

    if (keyFile !== undefined) {
        // On a ReplSet or mongos, our impersonation payload should be fine with cluster user.
        jsTest.log('Positive test, impersonation is okay when we\'re local.__system');

        authutil.asCluster(conn, keyFile, () => {
            assert.commandWorked(admin.runCommand(kImpersonatedUserHello));
            assert.commandWorked(admin.runCommand(kImpersonatedClientHello));
        });
    }

    jsTest.log('End');
}

{
    const standalone = MongoRunner.runMongod({auth: ''});
    runTest(standalone);
    MongoRunner.stopMongod(standalone);
}

{
    const kKeyfile = 'jstests/libs/key1';

    const rst = new ReplSetTest({nodes: 2});
    rst.startSet({keyFile: kKeyfile});
    rst.initiate();
    rst.awaitSecondaryNodes();
    runTest(rst.getPrimary(), kKeyfile);
    rst.stopSet();
}

{
    const kKeyfile = 'jstests/libs/key1';

    const st = new ShardingTest({
        mongos: 1,
        config: 1,
        shard: 2,
        keyFile: kKeyfile,
        other: {mongosOptions: {auth: null}, configOptions: {auth: null}, rsOptions: {auth: null}}
    });
    runTest(st.s0, kKeyfile);
    st.stop();
}
