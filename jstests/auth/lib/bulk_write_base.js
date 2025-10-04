// Auth test the BulkWrite command.
// These test cover privilege combination scenarios that commands_lib.js format cannot.
export function runTest(mongod) {
    const admin = mongod.getDB("admin");
    admin.createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});
    assert(admin.auth("admin", "pass"));

    // Establish test and test1
    mongod.getDB("test").coll.insert({x: "y"});
    mongod.getDB("test1").coll1.insert({x: "y"});

    admin.createRole({
        role: "ns1Insert",
        privileges: [{resource: {db: "test", collection: "coll"}, actions: ["insert"]}],
        roles: [],
    });

    admin.createRole({
        role: "ns2Insert",
        privileges: [{resource: {db: "test1", collection: "coll1"}, actions: ["insert"]}],
        roles: [],
    });

    admin.createRole({
        role: "ns1Update",
        privileges: [{resource: {db: "test", collection: "coll"}, actions: ["update"]}],
        roles: [],
    });

    admin.createRole({
        role: "ns2Update",
        privileges: [{resource: {db: "test1", collection: "coll1"}, actions: ["update"]}],
        roles: [],
    });

    admin.createRole({
        role: "ns1Remove",
        privileges: [{resource: {db: "test", collection: "coll"}, actions: ["remove"]}],
        roles: [],
    });

    admin.createRole({
        role: "ns2Remove",
        privileges: [{resource: {db: "test1", collection: "coll1"}, actions: ["remove"]}],
        roles: [],
    });

    admin.createRole({
        role: "ns1BypassDocumentValidation",
        privileges: [{resource: {db: "test", collection: "coll"}, actions: ["bypassDocumentValidation"]}],
        roles: [],
    });

    admin.createRole({
        role: "ns2BypassDocumentValidation",
        privileges: [{resource: {db: "test1", collection: "coll1"}, actions: ["bypassDocumentValidation"]}],
        roles: [],
    });

    // Create users to cover the scenarios where we have partial privileges on
    // byPassDocumentValidation and Insert + Update + Remove for ns1 + ns2.
    admin.createUser({
        user: "user1",
        pwd: "pass",
        roles: [
            "ns1Insert",
            "ns2Insert",
            "ns1Update",
            "ns2Update",
            "ns1Remove",
            "ns2Remove",
            "ns1BypassDocumentValidation",
        ],
    });
    admin.createUser({
        user: "user2",
        pwd: "pass",
        roles: ["ns1Insert", "ns1Update", "ns1Remove", "ns1BypassDocumentValidation", "ns2BypassDocumentValidation"],
    });
    admin.createUser({user: "user3", pwd: "pass", roles: ["ns1Update"]});

    admin.logout();

    // Commands to be used in testing.

    // Insert test.coll and test1.coll1 with bypassDocumentValidation.
    let cmd1 = {
        bulkWrite: 1,
        ops: [
            {insert: 0, document: {skey: "MongoDB"}},
            {insert: 1, document: {skey: "MongoDB"}},
        ],
        nsInfo: [{ns: "test.coll"}, {ns: "test1.coll1"}],
        bypassDocumentValidation: true,
    };

    let cmd2 = {
        bulkWrite: 1,
        ops: [
            {update: 0, filter: {skey: "MongoDB"}, updateMods: {field1: 1}},
            {update: 1, filter: {skey: "MongoDB"}, updateMods: {field1: 1}},
        ],
        nsInfo: [{ns: "test.coll"}, {ns: "test1.coll1"}],
        bypassDocumentValidation: true,
    };

    let cmd3 = {
        bulkWrite: 1,
        ops: [
            {delete: 0, filter: {skey: "MongoDB"}},
            {delete: 1, filter: {skey: "MongoDB"}},
        ],
        nsInfo: [{ns: "test.coll"}, {ns: "test1.coll1"}],
        bypassDocumentValidation: true,
    };

    let cmd4 = {
        bulkWrite: 1,
        ops: [{update: 0, filter: {skey: "MongoDB"}, updateMods: {field1: 1}, upsert: true}],
        nsInfo: [{ns: "test.coll"}],
    };

    const runAuthTest = function (test) {
        admin.auth(test.user, "pass");

        if (test.expectedAuthorized) {
            assert.commandWorked(admin.runCommand(test.command));
        } else {
            assert.commandFailedWithCode(admin.runCommand(test.command), [ErrorCodes.Unauthorized]);
        }
        admin.logout();
    };

    // Tests that insert fails authorization when fully authorized on ns1 and missing
    // 'bypassDocumentValidation' on ns2
    runAuthTest({user: "user1", command: cmd1, expectedAuthorized: false});

    // Tests that insert fails authorization when fully authorized on ns1 and missing 'insert' on
    // ns2
    runAuthTest({user: "user2", command: cmd1, expectedAuthorized: false});

    // Tests that update fails authorization when fully authorized on ns1 and missing
    // 'bypassDocumentValidation' on ns2
    runAuthTest({user: "user1", command: cmd2, expectedAuthorized: false});

    // Tests that update fails authorization when fully authorized on ns1 and missing 'update' on
    // ns2
    runAuthTest({user: "user2", command: cmd2, expectedAuthorized: false});

    // Tests that delete fails authorization when fully authorized on ns1 and missing
    // 'bypassDocumentValidation' on ns2
    runAuthTest({user: "user1", command: cmd3, expectedAuthorized: false});

    // Tests that delete fails authorization when fully authorized on ns1 and missing 'delete' on
    // ns2
    runAuthTest({user: "user2", command: cmd3, expectedAuthorized: false});

    // Tests that update with 'upsert: true' fails without 'insert' on ns1.
    runAuthTest({user: "user3", command: cmd4, expectedAuthorized: false});
}
