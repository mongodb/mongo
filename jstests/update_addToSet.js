
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


t.update( {} , { $addToSet : { a : { $each : [ 3 , 5 , 6 ] } } } );
