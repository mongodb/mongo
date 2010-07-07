
t = db.embeddedIndexTest;

t.remove( {} );

o = { name : "foo" , z : { a : 17 , b : 4} };
t.save( o );

assert( t.findOne().z.a == 17 );
assert( t.findOne( { z : { a : 17 } } ) == null);

t.ensureIndex( { "z.a" : 1 } );

assert( t.findOne().z.a == 17 );
assert( t.findOne( { z : { a : 17 } } ) == null);

o = { name : "bar" , z : { a : 18 } };
t.save( o );

assert.eq.automsg( "2", "t.find().length()" );
assert.eq.automsg( "2", "t.find().sort( { 'z.a' : 1 } ).length()" );
assert.eq.automsg( "2", "t.find().sort( { 'z.a' : -1 } ).length()" );
// We are planning to phase out this syntax.
assert( t.find().sort( { z : { a : 1 } } ).length() == 2 );
assert( t.find().sort( { z : { a: -1 } } ).length() == 2 );

//
// TODO - these don't work yet as indexing on x.y doesn't work yet
//
//assert( t.find().sort( { z : { a : 1 } } )[0].name == "foo" );
//assert( t.find().sort( { z : { a : -1 } } )[1].name == "bar" );

assert(t.validate().valid);
