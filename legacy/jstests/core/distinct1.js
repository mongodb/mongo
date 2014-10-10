
t = db.distinct1;
t.drop();

assert.eq( 0 , t.distinct( "a" ).length , "test empty" );

t.save( { a : 1 } )
t.save( { a : 2 } )
t.save( { a : 2 } )
t.save( { a : 2 } )
t.save( { a : 3 } )


res = t.distinct( "a" );
assert.eq( "1,2,3" , res.toString() , "A1" );

assert.eq( "1,2" , t.distinct( "a" , { a : { $lt : 3 } } ) , "A2" );

t.drop();

t.save( { a : { b : "a" } , c : 12 } );
t.save( { a : { b : "b" } , c : 12 } );
t.save( { a : { b : "c" } , c : 12 } );
t.save( { a : { b : "c" } , c : 12 } );

res = t.distinct( "a.b" );
assert.eq( "a,b,c" , res.toString() , "B1" );
printjson(t._distinct( "a.b" ).stats);
assert.eq( "COLLSCAN" , t._distinct( "a.b" ).stats.planSummary , "B2" )

t.drop();

t.save({_id: 1, a: 1});
t.save({_id: 2, a: 2});

// Test distinct with _id.
res = t.distinct( "_id" );
assert.eq( "1,2", res.toString(), "C1" );
res = t.distinct( "a", {_id: 1} );
assert.eq( "1", res.toString(), "C2" );
