
t = db.exists2;
t.drop();

t.save( { a : 1 , b : 1 } )
t.save( { a : 1 , b : 1 , c : 1 } )

assert.eq( 2 , t.find().itcount() , "A1" );
assert.eq( 2 , t.find( { a : 1 , b : 1 } ).itcount() , "A2" );
assert.eq( 1 , t.find( { a : 1 , b : 1 , c : { "$exists" : true } } ).itcount() , "A3" );

t.ensureIndex( { a : 1 , b : 1 , c : 1 } )
assert.eq( 1 , t.find( { a : 1 , b : 1 , c : { "$exists" : true } } ).itcount() , "B1" );

