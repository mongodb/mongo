// test read/write permissions

port = allocatePorts( 1 )[ 0 ];
baseName = "jstests_auth_auth1";

m = startMongod( "--auth", "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( "test" );

t = db[ baseName ];
t.drop();

users = db.getCollection( "system.users" );
users.remove( {} );

db.addUser( "eliot" , "eliot" );
db.addUser( "guest" , "guest", true );
db.getSisterDB( "admin" ).addUser( "super", "super" );

assert.throws( function() { t.findOne() }, [], "read without login" );

assert( db.auth( "eliot" , "eliot" ) , "auth failed" );

for( i = 0; i < 999; ++i ) {
    t.save( {i:i} );
}
assert.eq( 999, t.count() );
assert.eq( 999, t.find().toArray().length );

assert.eq( 999, db.eval( function() { return db[ "jstests_auth_auth1" ].count(); } ) );
db.eval( function() { db[ "jstests_auth_auth1" ].save( {i:999} ) } );
assert.eq( 1000, db.eval( function() { return db[ "jstests_auth_auth1" ].count(); } ) );

var p = { key : { i : true } , 
    reduce : function(obj,prev) { prev.count++; },
initial: { count: 0 }
};

assert.eq( 1000, t.group( p ).length );

assert( db.auth( "guest", "guest" ), "auth failed 2" );

assert.eq( 1000, t.count() );
assert.eq( 1000, t.find().toArray().length ); // make sure we have a getMore in play
assert.commandWorked( db.runCommand( {ismaster:1} ) );

assert( !db.getLastError() );
t.save( {} ); // fail
assert( db.getLastError() );
assert.eq( 1000, t.count() );

assert.eq( 2, db.system.users.count() );
assert( !db.getLastError() );
db.addUser( "a", "b" );
assert( db.getLastError() );
assert.eq( 2, db.system.users.count() );

assert.eq( 1000, db.eval( function() { return db[ "jstests_auth_auth1" ].count(); } ) );
assert.eq( 1000, db.eval( function() { return db[ "jstests_auth_auth1" ].find().toArray().length; } ) );
db.eval( function() { db[ "jstests_auth_auth1" ].save( {i:1} ) } );
assert.eq( 1000, db.eval( function() { return db[ "jstests_auth_auth1" ].count(); } ) );

assert.eq( 1000, t.group( p ).length );

var p = { key : { i : true } , 
    reduce : function(obj,prev) { db.jstests_auth_auth1.save( {i:10000} ); prev.count++; },
initial: { count: 0 }
};

assert.throws( function() { t.group( p ) } );
