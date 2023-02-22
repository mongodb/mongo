// Test that manually inserted impersonation can't escalate privileges.
// @tags: [requires_replication]

(function() {
'use strict';

function testMongod(mongod, systemuserpwd = undefined) {
    const admin = mongod.getDB('admin');
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
    const kImpersonatedHello = {
        hello: 1,
        "$audit": {
            "$impersonatedUser": {user: 'admin', db: 'admin'},
            "$impersonatedRoles": [{role: 'root', db: 'admin'}],
        }
    };
    assertError(
        kImpersonatedHello, 'Unauthorized use of impersonation metadata', ErrorCodes.Unauthorized);

    // TODO SERVER-72448: Remove
    const kImpersonatedHelloLegacy = {
        hello: 1,
        "$audit": {
            "$impersonatedUsers": [{user: 'admin', db: 'admin'}],
            "$impersonatedRoles": [{role: 'root', db: 'admin'}],
        }
    };
    assertError(kImpersonatedHelloLegacy,
                'Unauthorized use of impersonation metadata',
                ErrorCodes.Unauthorized);

    // TODO SERVER-72448: Remove, checks that both legacy and new impersonation metadata fields
    // cannot be set simultaneously.
    const kImpersonatedHelloBoth = {
        hello: 1,
        "$audit": {
            "$impersonatedUser": {user: 'admin', db: 'admin'},
            "$impersonatedUsers": [{user: 'admin', db: 'admin'}],
            "$impersonatedRoles": [{role: 'root', db: 'admin'}],
        }
    };
    assertError(kImpersonatedHelloBoth,
                'Cannot specify both $impersonatedUser and $impersonatedUsers',
                ErrorCodes.BadValue);

    // TODO SERVER-72448: Remove, checks that the legacy impersonation metadata field can only
    // contain at most 1 field if specified.
    const kImpersonatedHelloLegacyMultiple = {
        hello: 1,
        "$audit": {
            "$impersonatedUsers": [{user: 'admin', db: 'admin'}, {user: 'test', db: 'pwd'}],
            "$impersonatedRoles": [{role: 'root', db: 'admin'}],
        }
    };
    assertError(kImpersonatedHelloLegacyMultiple,
                'Can only impersonate up to one user per connection',
                ErrorCodes.BadValue);

    // Try as admin (root role), should still fail.
    admin.auth('admin', 'admin');
    assertError(
        kImpersonatedHello, 'Unauthorized use of impersonation metadata', ErrorCodes.Unauthorized);
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
