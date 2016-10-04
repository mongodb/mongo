// stat1.js
// test mongostat with authentication SERVER-3875
port = allocatePorts( 1 )[ 0 ];
baseName = "tool_stat1";

m = startMongod( "--auth", "--port", port, "--dbpath", MongoRunner.dataPath + baseName, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( "admin" );

t = db[ baseName ];
t.drop();

db.dropAllUsers();

db.createUser({user:  "eliot" , pwd: "eliot", roles: jsTest.adminUserRoles});

assert( db.auth( "eliot" , "eliot" ) , "auth failed" );

x = runMongoProgram( "mongostat", "--host", "127.0.0.1:"+port, "--username", "eliot", "--password", "eliot", "--rowcount", "1", "--authenticationDatabase=admin" );
assert.eq(x, 0, "mongostat should exit successfully with eliot:eliot");

x = runMongoProgram( "mongostat", "--host", "127.0.0.1:"+port, "--username", "eliot", "--password", "wrong", "--rowcount", "1", "--authenticationDatabase=admin" );
assert.eq(x, 1, "mongostat should exit with 1 with eliot:wrong");
