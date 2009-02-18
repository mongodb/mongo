

s = new ShardingTest( "movePrimary1" , 2 );

initDB = function( name ){
    var db = s.getDB( name );
    var c = db.foo;
    c.save( { a : 1 } );
    c.save( { a : 2 } );
    c.save( { a : 3 } );
    assert( 3 , c.count() );
    
    return s.getServer( name );
}

from = initDB( "test1" );
to = s.getOther( from );

assert.eq( 3 , from.getDB( "test1" ).foo.count() , "from doesn't have data before move" );
assert.eq( 0 , to.getDB( "test1" ).foo.count() , "to has data before move" );

assert.eq( s.config.databases.findOne( { name : "test1" } ).primary , from.name , "not in db correctly to start" );
s.admin.runCommand( { moveprimary : "test1" , to : to.name } );
assert.eq( s.config.databases.findOne( { name : "test1" } ).primary , to.name , "to in config db didn't change" );


assert.eq( 0 , from.getDB( "test1" ).foo.count() , "from still has data after move" );
assert.eq( 3 , to.getDB( "test1" ).foo.count() , "to doesn't have data after move" );

s.stop();

