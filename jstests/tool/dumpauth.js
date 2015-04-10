// dumpauth.js
// test mongodump with authentication
baseName = "tool_dumpauth";

var m = MongoRunner.runMongod({auth: "", bind_ip: "127.0.0.1"});
db = m.getDB( "admin" );

db.createUser({user:  "testuser" , pwd: "testuser", roles: jsTest.adminUserRoles});
assert( db.auth( "testuser" , "testuser" ) , "auth failed" );

t = db[ baseName ];
t.drop();

for(var i = 0; i < 100; i++) {
  t["testcol"].save({ "x": i });
}

x = runMongoProgram( "mongodump",
                     "--db", baseName,
                     "--authenticationDatabase=admin",
                     "-u", "testuser",
                     "-p", "testuser",
                     "-h", "127.0.0.1:"+m.port,
                     "--collection", "testcol" );
assert.eq(x, 0, "mongodump should succeed with authentication");

// SERVER-5233: mongodump with authentication breaks when using "--out -"
x = runMongoProgram( "mongodump",
                     "--db", baseName,
                     "--authenticationDatabase=admin",
                     "-u", "testuser",
                     "-p", "testuser",
                     "-h", "127.0.0.1:"+m.port,
                     "--collection", "testcol",
                     "--out", "-" );
assert.eq(x, 0, "mongodump should succeed with authentication while using '--out'");
