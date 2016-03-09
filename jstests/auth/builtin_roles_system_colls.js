// These tests cover any additional built-in role privileges
// that do not easily fit into the commands_lib.js framework.
// Specifically, they test the running of commands on the system
// collections such as system.users, etc.

// SERVER-13833: userAdminAnyDatabase role should be able to
// create and drop indexes on the admin.system.users and
// admin.system.roles collections, in order to make querying
// the users collection easier if you have a lot of users, etc.
function testUserAdminAnyDatabaseSystemCollIndexing(adminDB) {
    adminDB.auth("root", "pwd");
    adminDB.createUser({user: "king", pwd: "pwd", roles: ["userAdminAnyDatabase"]});
    adminDB.logout();

    adminDB.auth("king", "pwd");
    assert.commandWorked(adminDB.system.users.createIndex({db: 1}));
    assert.commandWorked(adminDB.system.roles.createIndex({db: 1}));
    assert.commandWorked(adminDB.system.users.dropIndex({db: 1}));
    assert.commandWorked(adminDB.system.roles.dropIndex({db: 1}));
    adminDB.logout();
}

// SERVER-14701: the backup role should be able to run the
// collstats command on all resouces, including system resources.
function testBackupSystemCollStats(adminDB) {
    adminDB.auth("root", "pwd");
    adminDB.createUser({user: "backup-agent", pwd: "pwd", roles: ["backup"]});
    adminDB.system.js.save({
        _id: "testFunction",
        value: function(x) {
            return x;
        }
    });
    adminDB.logout();

    adminDB.auth("backup-agent", "pwd");
    assert.commandWorked(adminDB.runCommand({collstats: "system.users"}));
    assert.commandWorked(adminDB.runCommand({collstats: "system.roles"}));
    assert.commandWorked(adminDB.runCommand({collstats: "system.js"}));
    adminDB.logout();
}

// ************************************************************

var conn = MongoRunner.runMongod({auth: ""});
var adminDB = conn.getDB("admin");
adminDB.createUser({user: "root", pwd: "pwd", roles: ["root"]});

testUserAdminAnyDatabaseSystemCollIndexing(adminDB);
testBackupSystemCollStats(adminDB);

MongoRunner.stopMongod(conn);
