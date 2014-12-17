
t = db.set2;
t.drop();

t.save( { _id : 1 , x : true , y : { x : true } } );
assert.eq( true , t.findOne().x );

t.update( { _id : 1 } , { $set : { x : 17 } } );
assert.eq( 17 , t.findOne().x );

assert.eq( true , t.findOne().y.x );
t.update( { _id : 1 } , { $set : { "y.x" : 17 } } );
assert.eq( 17 , t.findOne().y.x );

t.update( { _id : 1 } , { $set : { a : 2 , b : 3 } } );
assert.eq( 2 , t.findOne().a );
assert.eq( 3 , t.findOne().b );

