db = connect( "test" );
t = db.find1;
t.drop();

t.save( { a : 1 , b : "hi" } );
t.save( { a : 2 , b : "hi" } );

assert( t.findOne( { a : 1 } ).b != null , "A" );
assert( t.findOne( { a : 1 } , { a : 1 } ).b == null , "B");

assert( t.find( { a : 1 } )[0].b != null , "C" );
assert( t.find( { a : 1 } , { a : 1 } )[0].b == null , "D" );

id = t.findOne()._id;

assert( t.findOne( id ) , "E" );
assert( t.findOne( id ).a , "F" );
assert( t.findOne( id ).b , "G" );

assert( t.findOne( id , { a : 1 } ).a , "H" );
assert( ! t.findOne( id , { a : 1 } ).b ), "I" ;

assert(t.validate().valid);
