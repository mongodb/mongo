// test read/write permissions

port = allocatePorts( 1 )[ 0 ];
baseName = "jstests_auth_auth2";

m = startMongod( "--auth", "--port", port, "--dbpath", MongoRunner.dataPath + baseName, "--nohttpinterface", "--bind_ip", "127.0.0.1", "--nojournal", "--smallfiles" );
db = m.getDB( "admin" );

// These statements throw because the localhost exception does not allow
// these operations: it only allows the creation of the first admin user
// and necessary setup operations.
assert.throws( function(){ db.users.count(); } );
assert.throws( function() { db.shutdownServer(); } );

db.createUser( { user: "eliot", pwd: "eliot", roles: [ "root" ] } );

// These statements throw because we have a user but have not authenticated
// as that user.
assert.throws( function(){ db.users.count(); } );
assert.throws( function() { db.shutdownServer(); } );

db.auth( "eliot", "eliot" );

users = db.getCollection( "system.users" );
assert.eq( 1, users.count() );

db.shutdownServer();
