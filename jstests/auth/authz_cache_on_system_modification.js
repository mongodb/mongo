/**
 * This tests that the user cache is invalidated after any changes are made to system collections
 * @tags: [
 *   requires_fcv_72,
 * ]
 */

const conn = MongoRunner.runMongod({auth: ''});
let db = conn.getDB('admin');

// creates a root user
assert.commandWorked(db.runCommand({createUser: 'root', pwd: 'pwd', roles: ['__system']}),
                     "Could not create user 'admin'");

db = (new Mongo(conn.host)).getDB('admin');
db.auth('root', 'pwd');

// creates a unique role, a user who has that role, and a collection upon which they can exercise
// that role
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
db.logout();

// tests that a user does not retain their privileges after the system.roles collection is modified
(function testModifySystemRolesCollection() {
    jsTestLog("Testing authz cache invalidation on system.roles collection modification");
    assert(db.auth('custom', 'pwd'));
    assert.commandWorked(db.runCommand({insert: "admin.test", documents: [{foo: "bar"}]}),
                         "Could not insert to test collection with 'custom' user");
    db.logout();

    assert(db.auth('root', 'pwd'));
    assert.commandWorked(db.runCommand({renameCollection: "admin.system.roles", to: "admin.wolez"}),
                         "Could not rename system.roles collection with root user");
    db.logout();

    assert(db.auth('custom', 'pwd'));
    assert.commandFailedWithCode(
        db.runCommand({insert: "admin.test", documents: [{woo: "mar"}]}),
        ErrorCodes.Unauthorized,
        "Privileges retained after modification to system.roles collections");
    db.logout();
})();

// tests that a user cannot rename the system.users collection.
(function testModifySystemUsersCollection() {
    jsTestLog("Testing that a user cannot rename the system.users collection");
    assert(db.auth('root', 'pwd'));

    assert.commandFailedWithCode(
        db.runCommand({renameCollection: 'admin.system.users', to: 'foo.system.users'}),
        ErrorCodes.IllegalOperation,
        "Renaming the system.users collection should not be allowed");
    assert.commandFailedWithCode(
        db.runCommand({renameCollection: 'foo.system.users', to: 'admin.system.users'}),
        ErrorCodes.IllegalOperation,
        "Renaming the system.users collection should not be allowed");
    assert.commandFailedWithCode(
        db.runCommand({renameCollection: 'admin.system.users', to: 'admin.system.foo'}),
        ErrorCodes.IllegalOperation,
        "Renaming the system.users collection should not be allowed");
    assert.commandFailedWithCode(
        db.runCommand({renameCollection: 'admin.system.foo', to: 'admin.system.users'}),
        ErrorCodes.IllegalOperation,
        "Renaming the system.users collection should not be allowed");
    db.logout();
})();

MongoRunner.stopMongod(conn);
