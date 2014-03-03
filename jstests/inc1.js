
t = db.inc1;
t.drop();

function test( num , name ){
    assert.eq( 1 , t.count() , name + " count" );
    assert.eq( num , t.findOne().x , name + " value" );
}

t.save( { _id : 1 , x : 1 } );
test( 1 , "A" );

t.update( { _id : 1 } , { $inc : { x : 1 } } );
test( 2 , "B" );

t.update( { _id : 1 } , { $inc : { x : 1 } } );
test( 3 , "C" );

t.update( { _id : 2 } , { $inc : { x : 1 } } );
test( 3 , "D" );

t.update( { _id : 1 } , { $inc : { x : 2 } } );
test( 5 , "E" );

t.update( { _id : 1 } , { $inc : { x : -1 } } );
test( 4 , "F" );

t.ensureIndex( { x : 1 } );

t.update( { _id : 1 } , { $inc : { x : 1 } } );
test( 5 , "G" );

