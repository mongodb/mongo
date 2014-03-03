
t = db.type1;
t.drop();

t.save( { x : 1.1 } );
t.save( { x : "3" } );
t.save( { x : "asd" } );
t.save( { x : "foo" } );

assert.eq( 4 , t.find().count() , "A1" );
assert.eq( 1 , t.find( { x : { $type : 1 } } ).count() , "A2" );
assert.eq( 3 , t.find( { x : { $type : 2 } } ).count() , "A3" );
assert.eq( 0 , t.find( { x : { $type : 3 } } ).count() , "A4" );
assert.eq( 4 , t.find( { x : { $type : 1 } } ).explain().nscanned , "A5" );


t.ensureIndex( { x : 1 } );

assert.eq( 4 , t.find().count() , "B1" );
assert.eq( 1 , t.find( { x : { $type : 1 } } ).count() , "B2" );
assert.eq( 3 , t.find( { x : { $type : 2 } } ).count() , "B3" );
assert.eq( 0 , t.find( { x : { $type : 3 } } ).count() , "B4" );
assert.eq( 1 , t.find( { x : { $type : 1 } } ).explain().nscanned , "B5" );
assert.eq( 1 , t.find( { x : { $regex:"f", $type : 2 } } ).count() , "B3" );