
t = db.update_addToSet1;
t.drop();

o = { _id : 1 , a : [ 2 , 1 ] }
t.insert( o );

assert.eq( o , t.findOne() , "A1" );

t.update( {} , { $addToSet : { a : 3 } } );
o.a.push( 3 );
assert.eq( o , t.findOne() , "A2" );

t.update( {} , { $addToSet : { a : 3 } } );
assert.eq( o , t.findOne() , "A3" );

// SERVER-628
// t.update( {} , { $addToSet : { a : { $each : [ 3 , 5 , 6 ] } } } );



// SERVER-630
t.drop();
t.update( { _id : 2 } , { $addToSet : { a : 3 } } , true );
assert.eq( 1 , t.count() , "B1" );
assert.eq( { _id : 2 , a : [ 3 ] } , t.findOne() , "B2" );
