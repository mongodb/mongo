// test read/write permissions

print("START auth1.js");

port = allocatePorts( 1 )[ 0 ];
baseName = "jstests_auth_auth1";

m = startMongod( "--auth", "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( "test" );

t = db[ baseName ];
t.drop();

// these are used by read-only user
mro = new Mongo(m.host);
dbRO = mro.getDB( "test" );
tRO = dbRO[ baseName ];

users = db.getCollection( "system.users" );
users.remove( {} );

db.addUser( "eliot" , "eliot" );
db.addUser( "guest" , "guest", true );
db.getSisterDB( "admin" ).addUser( "super", "super" );

assert.throws( function() { t.findOne() }, [], "read without login" );

print("make sure we can't run certain commands w/out auth");
var errmsg = "need to login";
assert.eq(db.runCommand({eval : "function() { return 1; }"}).errmsg, errmsg);
assert.eq(db.runCommand({getLastError : 1}).errmsg, errmsg);
assert.eq(db.runCommand({whatsmyuri : 1}).errmsg, errmsg);
assert.eq(db.runCommand({availableQueryOptions : 1}).errmsg, errmsg);
assert.eq(db.adminCommand({getLog : "global"}).errmsg, errmsg);
assert.eq(db.runCommand({getPrevError : 1}).errmsg, errmsg);
assert.eq(db.runCommand({resetError : 1}).errmsg, errmsg);

assert( db.auth( "eliot" , "eliot" ) , "auth failed" );

for( i = 0; i < 999; ++i ) {
    t.save( {i:i} );
}
assert.eq( 999, t.count() , "A1" );
assert.eq( 999, t.find().toArray().length , "A2" );

db.setProfilingLevel( 2 );
t.count();
db.setProfilingLevel( 0 );
assert.lt( 0 , db.system.profile.find( { user : "eliot" } ).count() , "AP1" )

assert.eq( 999, db.eval( function() { return db[ "jstests_auth_auth1" ].count(); } ) , "A3" );
db.eval( function() { db[ "jstests_auth_auth1" ].save( {i:999} ) } );
assert.eq( 1000, db.eval( function() { return db[ "jstests_auth_auth1" ].count(); } ) , "A4" );

var p = { key : { i : true } , 
    reduce : function(obj,prev) { prev.count++; },
initial: { count: 0 }
};

assert.eq( 1000, t.group( p ).length , "A5" );

assert( dbRO.auth( "guest", "guest" ), "auth failed 2" );

assert.eq( 1000, tRO.count() , "B1" );
assert.eq( 1000, tRO.find().toArray().length , "B2" ); // make sure we have a getMore in play
assert.commandWorked( dbRO.runCommand( {ismaster:1} ) , "B3" );

assert( !dbRO.getLastError() , "B4" );
tRO.save( {} ); // fail
assert( dbRO.getLastError() , "B5: " + tojson( dbRO.getLastErrorObj() ) );
assert.eq( 1000, tRO.count() , "B6" );

// SERVER-4692 read-only users can't read system.users collection
assert.throws(function(){dbRO.system.users.findOne()});
assert.throws(function(){dbRO.system.users.count()});
assert( dbRO.getLastError() , "B6.5" );

assert.eq( 2, db.system.users.count() , "B7" ); // rw connection
assert.throws(function(){dbRO.addUser( "a", "b" )});
assert( dbRO.getLastError() , "B9" );
assert.eq( 2, db.system.users.count() , "B10"); // rw connection

assert.eq( 1000, dbRO.eval( function() { return db[ "jstests_auth_auth1" ].count(); } ) , "C1" );
assert.eq( 1000, dbRO.eval( function() { return db[ "jstests_auth_auth1" ].find().toArray().length; } ) , "C2" );
dbRO.eval( function() { db[ "jstests_auth_auth1" ].save( {i:1} ) } , "C3" );
assert.eq( 1000, dbRO.eval( function() { return db[ "jstests_auth_auth1" ].count(); } ) , "C4" );

assert.eq( 1000, tRO.group( p ).length , "C5" );

var p = { key : { i : true } , 
          reduce : function(obj,prev) { db.jstests_auth_auth1.save( {i:10000} ); prev.count++; },
          initial: { count: 0 }
        };

// this no longer throws (but the saves silently fail) SERVER-5228
//assert.throws( function() { return t.group( p ) }, null , "write reduce didn't fail" );
assert.eq( 1000, tRO.group( p ).length , "C6" );
assert.eq( 1000, dbRO.jstests_auth_auth1.count() , "C7" );

print("SUCCESS auth1.js");
