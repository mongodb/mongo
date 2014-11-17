// stat1.js
// test mongostat with authentication SERVER-3875
port = allocatePorts( 1 )[ 0 ];
baseName = "tool_stat1";

m = startMongod( "--auth", "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( "admin" );

t = db[ baseName ];
t.drop();

users = db.getCollection( "system.users" );
users.remove( {} );

db.addUser( "eliot" , "eliot" );

assert( db.auth( "eliot" , "eliot" ) , "auth failed" );

x = runMongoProgram( "mongostat", "--host", "127.0.0.1:"+port, "--username", "eliot", "--password", "eliot", "--rowcount", "1" );
assert.eq(x, 0, "mongostat should exit successfully with eliot:eliot");

x = runMongoProgram( "mongostat", "--host", "127.0.0.1:"+port, "--username", "eliot", "--password", "wrong", "--rowcount", "1" );
assert.eq(x, _isWindows() ? -1 : 255, "mongostat should exit with -1 with eliot:wrong");
