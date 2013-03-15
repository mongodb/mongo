// SERVER-8802: Test that you can't build indexes on system.users and use that to drop users with
// dropDups.
var conn = MongoRunner.runMongod({auth : ""});

function assertGLENotOK(status) {
    assert(status.ok && status.err !== null,
           "Expected not-OK status object; found " + tojson(status));
}

var adminDB = conn.getDB("admin");
var testDB = conn.getDB("test");
adminDB.addUser({user:'admin', pwd:'x', roles:['userAdminAnyDatabase']});
adminDB.auth('admin','x');
adminDB.addUser({user:'mallory', pwd:'x', roles:[], otherDBRoles:{test:['readWrite']}});
testDB.addUser({user:'user1', pwd:'x', roles:['read']});
testDB.addUser({user:'user2', pwd:'x', roles:['read']});
assert.eq(2, testDB.system.users.count());
adminDB.logout();

adminDB.auth('mallory', 'x');
testDB.system.users.createIndex({haxx:1}, {unique:true, dropDups:true});
assertGLENotOK(testDB.getLastErrorObj());
testDB.exploit.system.indexes.insert({ns: "test.system.users", key: { haxx: 1.0 }, name: "haxx_1",
                                      unique: true, dropDups: true});
assertGLENotOK(testDB.getLastErrorObj());
// Make sure that no indexes were built.
assert.eq(null,
          testDB.system.namespaces.findOne(
              {$and : [{name : /^test\.system\.users\.\$/},
                       {name : {$ne : "test.system.users.$_id_"}},
                       {name : {$ne : "test.system.users.$user_1_userSource_1"}} ]}));
adminDB.logout();

adminDB.auth('admin','x');
// Make sure that no users were actually dropped
assert.eq(2, testDB.system.users.count());