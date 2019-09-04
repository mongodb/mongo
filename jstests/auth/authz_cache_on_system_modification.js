/**
 * This tests that the user cache is invalidated after any changes are made to system collections
 */

(function() {
    "use strict";

    const conn = MongoRunner.runMongod({auth: ''});
    let db = conn.getDB('admin');
    const authzErrorCode = 13;

    // creates a root user
    assert.commandWorked(db.runCommand({createUser: 'root', pwd: 'pwd', roles: ['__system']}),
                         "Could not create user 'admin'");

    db = (new Mongo(conn.host)).getDB('admin');
    db.auth('root', 'pwd');

    // creates a unique role, a user who has that role, and a collection upon which they can
    // exercise that role
    assert.commandWorked(db.createCollection("admin.test", {}),
                         "Could not create test collection in admin db");
    assert.commandWorked(db.runCommand({
        createRole: 'writeCustom',
        roles: [],
        privileges: [{resource: {db: "admin", collection: "admin.test"}, actions: ["insert"]}]
    }),
                         "Could not create custom role");
    assert.commandWorked(db.runCommand({createUser: 'custom', pwd: 'pwd', roles: ['writeCustom']}),
                         "Could not create new user with custom role");

    // tests that a user does not retain their privileges after the system.roles collection is
    // modified
    (function testModifySystemRolesCollection() {
        jsTestLog("Testing authz cache invalidation on system.roles collection modification");
        assert(db.auth('custom', 'pwd'));
        assert.commandWorked(db.runCommand({insert: "admin.test", documents: [{foo: "bar"}]}),
                             "Could not insert to test collection with 'custom' user");
        assert(db.auth('root', 'pwd'));
        assert.commandWorked(
            db.runCommand({renameCollection: "admin.system.roles", to: "admin.wolez"}),
            "Could not rename system.roles collection with root user");
        assert(db.auth('custom', 'pwd'));
        assert.commandFailedWithCode(
            db.runCommand({insert: "admin.test", documents: [{woo: "mar"}]}),
            authzErrorCode,
            "Privileges retained after modification to system.roles collections");
    })();

    // tests that a user does not retain their privileges after the system.users colleciton is
    // modified
    (function testModifySystemUsersCollection() {
        jsTestLog("Testing authz cache invalidation on system.users collection modification");
        assert(db.auth('root', 'pwd'));
        assert.commandWorked(db.createCollection("scratch", {}),
                             "Collection not created with root user");
        assert.commandWorked(
            db.runCommand({renameCollection: 'admin.system.users', to: 'admin.foo'}),
            "System collection could not be renamed with root user");
        assert.commandFailedWithCode(
            db.runCommand({renameCollection: 'admin.scratch', to: 'admin.system.users'}),
            authzErrorCode,
            "User cache not invalidated after modification to system collection");
    })();

    MongoRunner.stopMongod(conn);
})();