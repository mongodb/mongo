// dumpauth.js
// test mongodump with authentication

var m = MongoRunner.runMongod({auth: "", bind_ip: "127.0.0.1"});
var dbName = "admin"
var colName = "testcol"
db = m.getDB(dbName);

db.createUser({user:  "testuser" , pwd: "testuser", roles: jsTest.adminUserRoles});
assert( db.auth( "testuser" , "testuser" ) , "auth failed" );

t = db[colName];
t.drop();

for(var i = 0; i < 100; i++) {
  t.save({ "x": i });
}

x = runMongoProgram( "mongodump",
                     "--db", dbName,
                     "--authenticationDatabase=admin",
                     "-u", "testuser",
                     "-p", "testuser",
                     "-h", "127.0.0.1:"+m.port,
                     "--collection", colName);
assert.eq(x, 0, "mongodump should succeed with authentication");
