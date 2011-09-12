// test read/write permissions

port = allocatePorts( 1 )[ 0 ];
baseName = "jstests_auth_auth2";

m = startMongod( "--auth", "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface", "--bind_ip", "127.0.0.1" , "--nojournal" , "--smallfiles" );
db = m.getDB( "admin" );

t = db[ baseName ];
t.drop();

users = db.getCollection( "system.users" );
assert.eq( 0 , users.count() );

db.addUser( "eliot" , "eliot" );

assert.throws( function(){ db.users.count(); } )

assert.throws( function() { db.shutdownServer(); } )

db.auth( "eliot" , "eliot" )

db.shutdownServer();
