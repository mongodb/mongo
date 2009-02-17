


a = startMongod( { port : 30000 , dbpath : "/data/db/diffservers1a" } )
b = startMongod( { port : 30001 , dbpath : "/data/db/diffservers1b" } )

s = startMongos( { port : 30002 , configdb : "localhost:30000" } );

config = s.getDB( "config" );
admin = s.getDB( "admin" );

admin.runCommand( { addserver : "localhost:30000" } )
admin.runCommand( { addserver : "localhost:30001" } )

assert.eq( 2 , config.servers.count() , "server count wrong" );
assert.eq( 2 , a.getDB( "config" ).servers.count() , "where are servers!" );
assert.eq( 0 , b.getDB( "config" ).servers.count() , "shouldn't be here" );

test1 = s.getDB( "test1" ).foo;
test1.save( { a : 1 } );
test1.save( { a : 2 } );
test1.save( { a : 3 } );
assert( 3 , test1.count() );

stopMongoProgram( 30002 );
stopMongod( 30000 );
stopMongod( 30001 );

