baseName = "jstests_auth_auth2";

port = allocatePorts( 1 )[ 0 ];
m = startMongod( "--auth", "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( "admin" );

// No passwords yet so should be true.
assert( db.isAuth(), "isAuth should have pass on no users")

// Add a user
db.addUser( "admin" , "super" );


assert( ! db.isAuth(), "isAuth should have failed if unauthenteicated")

db.auth("admin","super")

assert( db.isAuth(), "isauth should have passed after authentication" );

db.runCommand("logout");

assert( ! db.isAuth(), "isAuth after logout should have failed")
