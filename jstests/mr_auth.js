// MapReduce should not be able to override an existing result table if the user does not have write permission when --auth enabled. SERVER-3345

baseName = "jstests_mr_auth";
dbName = "test";
out = baseName + "_out";

map = function(){ emit( this.x, this.y );}
red = function( k, vs ){ var s=0; for (var i=0; i<vs.length; i++) s+=vs[i]; return s;}
red2 = function( k, vs ){ return 42;}

ports = allocatePorts( 2 );

// make sure writing is allowed when started without --auth enabled

port = ports[ 0 ];
dbms = startMongod( "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = dbms.getDB( dbName );
t = db[ baseName ];

for( var i = 0; i < 1000; i++) t.insert( {_id:i, x:i%10, y:i%100} );
assert.eq( 1000, t.count(), "inserts failed" );

db.system.users.remove( {} );
db.addUser( "write" , "write" );
db.addUser( "read" , "read", true );
db.getSisterDB( "admin" ).addUser( "admin", "admin" );

t.mapReduce( map, red, {out: { inline: 1 }} )

t.mapReduce( map, red, {out: { replace: out }} )
t.mapReduce( map, red, {out: { reduce: out }} )
t.mapReduce( map, red, {out: { merge: out }} )

db[ out ].drop();

stopMongod( port );


// In --auth mode, read-only user should not be able to output to existing collection

port = ports[ 1 ];
dbms = startMongodNoReset( "--auth", "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = dbms.getDB( dbName );
t = db[ baseName ];

assert.throws( function() { t.findOne() }, [], "read without login" );

assert.throws( function(){ t.mapReduce( map, red, {out: { inline: 1 }} ) }, [], "m/r without login" );


db.auth( "read", "read" );

t.findOne()

t.mapReduce( map, red, {out: { inline: 1 }} )

t.mapReduce( map, red, {out: { replace: out }} )
docs = db[ out ].find().toArray();

assert.throws( function(){ t.mapReduce( map, red2, {out: { replace: out }} ) }, [], "read-only user shouldn't be able to output m/r to existing collection (created by previous m/r)" );
assert.throws( function(){ t.mapReduce( map, red2, {out: { reduce: out }} ) }, [], "read-only user shouldn't be able to output m/r to existing collection (created by previous m/r)" );

docs2 = db[ out ].find().toArray();
assert.eq (docs, docs2, "output collection updated even though exception was raised");

db.logout();

assert.throws( function(){ t.mapReduce( map, red, {out: { replace: out }} ) }, [], "m/r without login" );


db.auth( "write", "write" )

t.mapReduce( map, red, {out: { inline: 1 }} )

t.mapReduce( map, red, {out: { replace: out }} )
t.mapReduce( map, red, {out: { reduce: out }} )
t.mapReduce( map, red, {out: { merge: out }} )

stopMongod( port );
