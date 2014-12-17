
t = db.inc2
t.drop();

t.save( { _id : 1 , x : 1 } );
t.save( { _id : 2 , x : 2 } );
t.save( { _id : 3 , x : 3 } );

function order(){
    return t.find().sort( { x : 1 } ).map( function(z){ return z._id; } );
}

assert.eq( "1,2,3" , order() , "A" );

t.update( { _id : 1 } , { $inc : { x : 4 } } );
assert.eq( "2,3,1" , order() , "B" );

t.ensureIndex( { x : 1 } );
assert.eq( "2,3,1" , order() , "C" );

t.update( { _id : 3 } , { $inc : { x : 4 } } );
assert.eq( "2,1,3" , order() , "D" );
