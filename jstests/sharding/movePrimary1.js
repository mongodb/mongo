


a = startMongod( { port : 30000 , dbpath : "/data/db/movePrimary1a" } )
b = startMongod( { port : 30001 , dbpath : "/data/db/movePrimary1b" } )

s = startMongos( { port : 30002 , configdb : "localhost:30000" } );

config = s.getDB( "config" );
admin = s.getDB( "admin" );

admin.runCommand( { addserver : "localhost:30000" } )
admin.runCommand( { addserver : "localhost:30001" } )

initDB = function( name ){
    var db = s.getDB( name );
    var c = db.foo;
    c.save( { a : 1 } );
    c.save( { a : 2 } );
    c.save( { a : 3 } );
    assert( 3 , c.count() );
    
    return config.databases.findOne( { name : name } ).primary;
}

from = initDB( "test1" );
if ( from == "localhost:30000" ){
    to = "localhost:30001";
    fromMongo = a;
    toMongo = b;
}
else if ( from == "localhost:30001" ){
    to = "localhost:30000";
    fromMongo = b;
    toMongo = a;
}
else 
    throw "what: " + from;

assert.eq( 3 , fromMongo.getDB( "test1" ).foo.count() , "from doesn't have data before move" );
assert.eq( 0 , toMongo.getDB( "test1" ).foo.count() , "to has data before move" );

admin.runCommand( { moveprimary : "test1" , to : to } );
assert.eq( config.databases.findOne( { name : "test1" } ).primary , to , "to in config db didn't change" );


assert.eq( 0 , fromMongo.getDB( "test1" ).foo.count() , "from still has data after move" );
assert.eq( 3 , toMongo.getDB( "test1" ).foo.count() , "to doesn't have data after move" );

stopMongoProgram( 30002 );
stopMongod( 30000 );
stopMongod( 30001 );

