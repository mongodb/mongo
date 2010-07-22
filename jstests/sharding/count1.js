// count1.js

s = new ShardingTest( "count1" , 2 , 10 );
db = s.getDB( "test" );

db.bar.save( { n : 1 } )
db.bar.save( { n : 2 } )
db.bar.save( { n : 3 } )

assert.eq( 3 , db.bar.find().count() , "bar 1" );
assert.eq( 1 , db.bar.find( { n : 1 } ).count() , "bar 2" );

s.adminCommand( { enablesharding : "test" } )
s.adminCommand( { shardcollection : "test.foo" , key : { name : 1 } } );

primary = s.getServer( "test" ).getDB( "test" );
secondary = s.getOther( primary ).getDB( "test" );

assert.eq( 1 , s.config.chunks.count() , "sanity check A" );

db.foo.save( { name : "eliot" } )
db.foo.save( { name : "sara" } )
db.foo.save( { name : "bob" } )
db.foo.save( { name : "joe" } )
db.foo.save( { name : "mark" } )
db.foo.save( { name : "allan" } )

assert.eq( 6 , db.foo.find().count() , "basic count" );

s.adminCommand( { split : "test.foo" , find : { name : "joe" } } );
s.adminCommand( { split : "test.foo" , find : { name : "joe" } } );
s.adminCommand( { split : "test.foo" , find : { name : "joe" } } );

assert.eq( 6 , db.foo.find().count() , "basic count after split " );
assert.eq( 6 , db.foo.find().sort( { name : 1 } ).count() , "basic count after split sorted " );

s.adminCommand( { movechunk : "test.foo" , find : { name : "joe" } , to : secondary.getMongo().name } );


assert.eq( 3 , primary.foo.find().toArray().length , "primary count" );
assert.eq( 3 , secondary.foo.find().toArray().length , "secondary count" );
assert.eq( 3 , primary.foo.find().sort( { name : 1 } ).toArray().length , "primary count sorted" );
assert.eq( 3 , secondary.foo.find().sort( { name : 1 } ).toArray().length , "secondary count sorted" );

assert.eq( 6 , db.foo.find().toArray().length , "total count after move" );
assert.eq( 6 , db.foo.find().sort( { name : 1 } ).toArray().length , "total count() sorted" );

assert.eq( 6 , db.foo.find().sort( { name : 1 } ).count() , "total count with count() after move" );

assert.eq( "allan,bob,eliot,joe,mark,sara" ,  db.foo.find().sort( { name : 1 } ).toArray().map( function(z){ return z.name; } ) , "sort 1" );
assert.eq( "sara,mark,joe,eliot,bob,allan" ,  db.foo.find().sort( { name : -1 } ).toArray().map( function(z){ return z.name; } ) , "sort 2" );

assert.eq( 2 , db.foo.find().limit(2).itcount() , "LS1" )
assert.eq( 2 , db.foo.find().skip(2).limit(2).itcount() , "LS2" )
assert.eq( 1 , db.foo.find().skip(5).limit(2).itcount() , "LS3" )
assert.eq( 6 , db.foo.find().limit(2).count() , "LSC1" )
assert.eq( 2 , db.foo.find().limit(2).size() , "LSC2" )
assert.eq( 2 , db.foo.find().skip(2).limit(2).size() , "LSC3" )
assert.eq( 1 , db.foo.find().skip(5).limit(2).size() , "LSC4" )

s.stop();


