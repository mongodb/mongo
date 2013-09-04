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
adminDB.addUser({user:'mallory', pwd:'x', roles:['readWriteAnyDatabase']});
testDB.addUser({user:'user', pwd:'x', roles:['read']});
assert.eq(3, adminDB.system.users.count());
adminDB.logout();

adminDB.auth('mallory', 'x');
adminDB.system.users.createIndex({haxx:1}, {unique:true, dropDups:true});
assertGLENotOK(adminDB.getLastErrorObj());
adminDB.exploit.system.indexes.insert({ns: "admin.system.users", key: { haxx: 1.0 }, name: "haxx_1",
                                       unique: true, dropDups: true});
assertGLENotOK(testDB.getLastErrorObj());
// Make sure that no indexes were built.
assert.eq(null,
          adminDB.system.namespaces.findOne(
              {$and : [{name : /^admin\.system\.users\.\$/},
                       {name : {$ne : "admin.system.users.$_id_"}},
                       {name : {$ne : "admin.system.users.$name_1_source_1"}} ]}));
adminDB.logout();

adminDB.auth('admin','x');
// Make sure that no users were actually dropped
assert.eq(3, adminDB.system.users.count());