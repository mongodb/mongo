// SERVER-8802: Test that you can't build indexes on system.users and use that to drop users with
// dropDups.
var conn = MongoRunner.runMongod({auth: ""});

var adminDB = conn.getDB("admin");
var testDB = conn.getDB("test");
adminDB.createUser({user: 'admin', pwd: 'x', roles: ['userAdminAnyDatabase']});
adminDB.auth('admin', 'x');
adminDB.createUser({user: 'mallory', pwd: 'x', roles: ['readWriteAnyDatabase']});
testDB.createUser({user: 'user', pwd: 'x', roles: ['read']});
assert.eq(3, adminDB.system.users.count());
adminDB.logout();

adminDB.auth('mallory', 'x');
var res = adminDB.system.users.createIndex({haxx: 1}, {unique: true, dropDups: true});
assert(!res.ok);
assert.eq(13, res.code);  // unauthorized
assert.writeError(adminDB.exploit.system.indexes.insert(
    {ns: "admin.system.users", key: {haxx: 1.0}, name: "haxx_1", unique: true, dropDups: true}));
// Make sure that no indexes were built.
var collectionInfosCursor = adminDB.runCommand("listCollections", {
    filter: {
        $and: [
            {name: /^admin\.system\.users\.\$/},
            {name: {$ne: "admin.system.users.$_id_"}},
            {name: {$ne: "admin.system.users.$user_1_db_1"}}
        ]
    }
});

assert.eq([], new DBCommandCursor(adminDB.getMongo(), collectionInfosCursor).toArray());
adminDB.logout();

adminDB.auth('admin', 'x');
// Make sure that no users were actually dropped
assert.eq(3, adminDB.system.users.count());
