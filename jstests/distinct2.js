
t = db.distinct2;
t.drop();

t.save({a:null});
assert.eq( 0 , t.distinct('a.b').length , "A" );

t.drop();
t.save( { a : 1 } );
assert.eq( [1] , t.distinct( "a" ) , "B" );
t.save( {} )
assert.eq( [1] , t.distinct( "a" ) , "C" );

