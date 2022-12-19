'use strict';

// Auth test the BulkWrite command.
// These test cover privilege combination scenarios that commands_lib.js format cannot.
function runTest(mongod) {
    load("jstests/libs/feature_flag_util.js");

    const admin = mongod.getDB('admin');
    admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
    assert(admin.auth('admin', 'pass'));

    // Skip this test if the BulkWriteCommand feature flag is not enabled.
    if (!FeatureFlagUtil.isEnabled(admin, "BulkWriteCommand")) {
        jsTestLog('Skipping test because the BulkWriteCommand feature flag is disabled.');
        admin.logout();
        return;
    }

    // Establish test and test1
    mongod.getDB("test").coll.insert({x: "y"});
    mongod.getDB("test1").coll1.insert({x: "y"});

    admin.createRole({
        role: 'ns1Insert',
        privileges: [{resource: {db: "test", collection: "coll"}, actions: ['insert']}],
        roles: []
    });

    admin.createRole({
        role: 'ns2Insert',
        privileges: [{resource: {db: "test1", collection: "coll1"}, actions: ['insert']}],
        roles: []
    });

    admin.createRole({
        role: 'ns1BypassDocumentValidation',
        privileges:
            [{resource: {db: "test", collection: "coll"}, actions: ['bypassDocumentValidation']}],
        roles: []
    });

    admin.createRole({
        role: 'ns2BypassDocumentValidation',
        privileges:
            [{resource: {db: "test1", collection: "coll1"}, actions: ['bypassDocumentValidation']}],
        roles: []
    });

    // Create users to cover the scenarios where we have partial privileges on
    // byPassDocumentValidation and Insert for ns1 + ns2.
    admin.createUser({
        user: 'user1',
        pwd: 'pass',
        roles: ['ns1Insert', 'ns2Insert', 'ns1BypassDocumentValidation']
    });
    admin.createUser({
        user: 'user2',
        pwd: 'pass',
        roles: ['ns1Insert', 'ns1BypassDocumentValidation', 'ns2BypassDocumentValidation']
    });
    admin.logout();

    // Commands to be used in testing.

    // Insert test.coll and test1.coll1 with bypassDocumentValidation.
    var cmd1 = {
        bulkWrite: 1,
        ops: [{insert: 0, document: {skey: "MongoDB"}}, {insert: 1, document: {skey: "MongoDB"}}],
        nsInfo: [{ns: "test.coll"}, {ns: "test1.coll1"}],
        bypassDocumentValidation: true,
    };

    const runAuthTest = function(test) {
        admin.auth(test.user, 'pass');

        if (test.expectedAuthorized) {
            assert.commandWorked(admin.runCommand(test.command));
        } else {
            assert.commandFailedWithCode(admin.runCommand(test.command), [ErrorCodes.Unauthorized]);
        }
        admin.logout();
    };

    // Tests that we fail authorization when fully authorized on ns1 and missing 'insert' on ns2
    runAuthTest({user: "user1", command: cmd1, expectedAuthorized: false});

    // Tests that we fail authorization when fully authorized on ns1 and missing
    // 'bypassDocumentValidation' on ns2
    runAuthTest({user: "user2", command: cmd1, expectedAuthorized: false});
}
