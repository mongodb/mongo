// dumpauth.js
// test mongodump with authentication
port = allocatePorts( 1 )[ 0 ];
baseName = "tool_dumpauth";

m = startMongod( "--auth", "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( "admin" );

t = db[ baseName ];
t.drop();

for(var i = 0; i < 100; i++) {
  t["testcol"].save({ "x": i });
}

users = db.getCollection( "system.users" );

db.addUser( "testuser" , "testuser" );

assert( db.auth( "testuser" , "testuser" ) , "auth failed" );

x = runMongoProgram( "mongodump",
                     "--db", baseName,
                     "--authenticationDatabase=admin",
                     "-u", "testuser",
                     "-p", "testuser",
                     "-h", "127.0.0.1:"+port,
                     "--collection", "testcol" );
assert.eq(x, 0, "mongodump should succeed with authentication");
