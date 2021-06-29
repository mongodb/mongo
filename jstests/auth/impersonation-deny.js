// Test that manually inserted impersonation can't escalate privileges.
// @tags: [requires_replication]

(function() {
'use strict';

function testMongod(mongod, systemuserpwd = undefined) {
    const admin = mongod.getDB('admin');
    admin.createUser({user: 'admin', pwd: 'admin', roles: ['root']});

    function assertUnauthorized(cmd, msg) {
        const errmsg =
            assert.commandFailedWithCode(admin.runCommand(cmd), ErrorCodes.Unauthorized).errmsg;
        assert(errmsg.includes(msg), "Error message is missing '" + msg + "': " + errmsg);
    }

    jsTest.log('Start - Sanity check without impersonation metadata');

    // Localhost authbypass is disabled, and we haven't logged in,
    // so normal auth-required commands should fail.
    assertUnauthorized({usersInfo: 1}, 'command usersInfo requires authentication');

    // Hello command requires no auth, so it works fine.
    assert.commandWorked(admin.runCommand({hello: 1}));

    jsTest.log('Negative tests - Add impersonation metadata to hello command');

    // Adding impersonation metadata is forbidden if we're not permitted to use it.
    const kImpersonatedHello = {
        hello: 1,
        "$audit": {
            "$impersonatedUsers": [{user: 'admin', db: 'admin'}],
            "$impersonatedRoles": [{role: 'root', db: 'admin'}],
        }
    };
    assertUnauthorized(kImpersonatedHello, 'Unauthorized use of impersonation metadata');

    // Try as admin (root role), should still fail.
    admin.auth('admin', 'admin');
    assertUnauthorized(kImpersonatedHello, 'Unauthorized use of impersonation metadata');
    admin.logout();

    if (systemuserpwd !== undefined) {
        // On a ReplSet, our impersonation payload should be fine with cluster user.
        jsTest.log('Positive test, impersonation is okay when we\'re local.__system');

        const local = mongod.getDB('local');
        local.auth('__system', systemuserpwd);
        assert.commandWorked(admin.runCommand(kImpersonatedHello));
        local.logout();
    }

    jsTest.log('End');
}

{
    const standalone = MongoRunner.runMongod({auth: ''});
    testMongod(standalone);
    MongoRunner.stopMongod(standalone);
}

{
    const kKeyfile = 'jstests/libs/key1';
    const kKey = cat(kKeyfile).replace(/[\011-\015\040]/g, '');

    const rst = new ReplSetTest({nodes: 2});
    rst.startSet({keyFile: kKeyfile});
    rst.initiate();
    rst.awaitSecondaryNodes();
    testMongod(rst.getPrimary(), kKey);
    rst.stopSet();
}
})();
