// These tests cover any additional built-in role privileges
// that do not easily fit into the commands_lib.js framework.
// Specifically, they test the running of commands on the system
// collections such as system.users, etc.

// SERVER-13833: userAdminAnyDatabase role should be able to
// create and drop indexes on the admin.system.users and
// admin.system.roles collections, in order to make querying
// the users collection easier if you have a lot of users, etc.
function testUserAdminAnyDatabaseSystemCollIndexing(conn) {
    var adminDB = conn.getDB("admin");
    adminDB.createUser({ user: "king", pwd: "pwd", roles: ["userAdminAnyDatabase"] });
    adminDB.auth("king", "pwd");

    assert.commandWorked(adminDB.system.users.createIndex({ db: 1 }));
    assert.commandWorked(adminDB.system.roles.createIndex({ db: 1 }));
    assert.commandWorked(adminDB.system.users.dropIndex({ db: 1 }));
    assert.commandWorked(adminDB.system.roles.dropIndex({ db: 1 }));
};

// ************************************************************

var conn = MongoRunner.runMongod({ auth: "" });
testUserAdminAnyDatabaseSystemCollIndexing(conn);
MongoRunner.stopMongod(conn);
