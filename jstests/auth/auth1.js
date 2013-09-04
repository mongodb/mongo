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

db.removeAllUsers();

db.getSisterDB( "admin" ).addUser( "super", "super", jsTest.adminUserRoles );
db.getSisterDB("admin").auth("super", "super");
db.addUser( "eliot" , "eliot", jsTest.basicUserRoles );
db.addUser( "guest" , "guest", jsTest.readOnlyUserRoles );
db.getSisterDB("admin").logout();

assert.throws( function() { t.findOne() }, [], "read without login" );

print("make sure we can't run certain commands w/out auth");
var codeUnauthorized = 13;
var rslt = db.runCommand({eval : "function() { return 1; }"});
assert.eq(rslt.code, codeUnauthorized, tojson(rslt));
var rslt = db.runCommand({getLog : "global"});
assert.eq(rslt.code, codeUnauthorized, tojson(rslt));

assert(!db.auth("eliot", "eliot2"), "auth succeeded with wrong password");
assert(db.auth("eliot", "eliot"), "auth failed");
// Change password
db.changeUserPassword("eliot", "eliot2");
assert(!db.auth("eliot", "eliot"), "auth succeeded with wrong password");
assert(db.auth("eliot", "eliot2"), "auth failed");

for( i = 0; i < 1000; ++i ) {
    t.save( {i:i} );
}
assert.eq( 1000, t.count() , "A1" );
assert.eq( 1000, t.find().toArray().length , "A2" );

db.setProfilingLevel( 2 );
t.count();
db.setProfilingLevel( 0 );
assert.lt( 0 , db.system.profile.find( { user : "eliot@test" } ).count() , "AP1" )

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

assert.eq( 1000, tRO.group( p ).length , "C1" );

var p = { key : { i : true } , 
          reduce : function(obj,prev) { db.jstests_auth_auth1.save( {i:10000} ); prev.count++; },
          initial: { count: 0 }
        };


assert.throws( function() { return t.group( p ) }, null , "write reduce didn't fail" );
assert.eq( 1000, dbRO.jstests_auth_auth1.count() , "C3" );


db.getSiblingDB('admin').auth('super', 'super');

assert.eq( 1000, db.eval( function() { return db[ "jstests_auth_auth1" ].count(); } ) , "D1" );
db.eval( function() { db[ "jstests_auth_auth1" ].save( {i:1000} ) } );
assert.eq( 1001, db.eval( function() { return db[ "jstests_auth_auth1" ].count(); } ) , "D2" );


print("SUCCESS auth1.js");
