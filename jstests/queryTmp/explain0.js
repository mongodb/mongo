// basic test for explain.n field
// refer to http://docs.mongodb.org/manual/reference/method/cursor.explain
// at the bare minimum, the query framework should be
// able to fill in this field correctly when performing a
// a collection scan on a single document collection

t = db.explain0;
t.drop();

t.save( { x : 0 } );

q = {};

assert.eq( 1 , t.find( q ).count() , "A" );
assert.eq( 1 , t.find( q ).itcount() , "B" );
assert.eq( 1 , t.find( q ).explain().n , "C" );
