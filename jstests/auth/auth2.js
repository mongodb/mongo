// test --authWrite mode

port = allocatePorts( 1 )[ 0 ];
baseName = "jstests_auth_auth2";

m = startMongod( "--authWriteOnly", "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( "test" );

t = db[ baseName ];
t.drop();

users = db.getCollection( "system.users" );
users.remove( {} );

for( i = 0; i < 1000; ++i ) {
    t.save( {i:i} );
}
assert.eq( 1000, t.count() );

db.getSisterDB( "admin" ).addUser( "super", "super" );

assert.eq( 1000, t.count() );
assert.eq( 1000, t.find().toArray().length ); // make sure we have a getMore in play
assert.commandWorked( db.runCommand( {ismaster:1} ) );

assert( !db.getLastError() );
t.save( {} ); // fail
assert( db.getLastError() );
assert.eq( 1000, t.count() );

assert.eq( 0, db.system.users.count() );
assert( !db.getLastError() );
db.addUser( "a", "b" );
assert( db.getLastError() );
assert.eq( 0, db.system.users.count() );