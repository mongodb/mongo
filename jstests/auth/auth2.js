baseName = "jstests_auth_auth2";

port = allocatePorts( 1 )[ 0 ];
m = startMongod( "--auth", "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( "admin" );

// No passwords yet so should be true.
assert( db.isAuthed(), "isAuthed should have pass on no users")

// Add a user
db.addUser( "admin" , "super" );

assert( ! db.isAuthed(), "isAuthed should have failed if unauthenticated")

db.auth("admin","super")

assert( db.isAuthed(), "isAuthed should have passed after authentication" );

db.runCommand("logout");

assert( ! db.isAuthed(), "isAuthed after logout should have failed")

