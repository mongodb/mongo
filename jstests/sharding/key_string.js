// key_string.js

s = new ShardingTest( "keystring" , 2 );

db = s.getDB( "test" );
s.adminCommand( { enablesharding : "test" } )
s.adminCommand( { shardcollection : "test.foo" , key : { name : 1 } } );

primary = s.getServer( "test" ).getDB( "test" );
seconday = s.getOther( primary ).getDB( "test" );

assert.eq( 1 , s.config.chunks.count() , "sanity check A" );

db.foo.save( { name : "eliot" } )
db.foo.save( { name : "sara" } )
db.foo.save( { name : "bob" } )
db.foo.save( { name : "joe" } )
db.foo.save( { name : "mark" } )
db.foo.save( { name : "allan" } )

assert.eq( 6 , db.foo.find().count() , "basic count" );

s.adminCommand( { split : "test.foo" , find : { name : "joe" } } ); // [Minkey -> allan) , * [allan -> ..)
s.adminCommand( { split : "test.foo" , find : { name : "joe" } } ); // * [allan -> sara) , [sara -> Maxkey)
s.adminCommand( { split : "test.foo" , find : { name : "joe" } } ); // [alan -> joe) , [joe -> sara]

s.adminCommand( { movechunk : "test.foo" , find : { name : "allan" } , to : seconday.getMongo().name } );

s.printChunks();

assert.eq( 3 , primary.foo.find().toArray().length , "primary count" );
assert.eq( 3 , seconday.foo.find().toArray().length , "secondary count" );

assert.eq( 6 , db.foo.find().toArray().length , "total count" );
assert.eq( 6 , db.foo.find().sort( { name : 1 } ).toArray().length , "total count sorted" );

assert.eq( 6 , db.foo.find().sort( { name : 1 } ).count() , "total count with count()" );

assert.eq( "allan,bob,eliot,joe,mark,sara" ,  db.foo.find().sort( { name : 1 } ).toArray().map( function(z){ return z.name; } ) , "sort 1" );
assert.eq( "sara,mark,joe,eliot,bob,allan" ,  db.foo.find().sort( { name : -1 } ).toArray().map( function(z){ return z.name; } ) , "sort 2" );

// make sure we can't foce a split on an extreme key
// [allan->joe) 
assert.throws( function(){ s.adminCommand( { split : "test.foo" , middle : { name : "allan" } } ) } );
assert.throws( function(){ s.adminCommand( { split : "test.foo" , middle : { name : "joe" } } ) } );

s.stop();


