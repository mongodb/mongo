
t = db.update7;
t.drop();

function s(){
    return t.find().sort( { _id : 1 } ).map( function(z){ return z.x; } );
}

t.save( { _id : 1 , x : 1 } );
t.save( { _id : 2 , x : 5 } );

assert.eq( "1,5" , s() , "A" );

t.update( {} , { $inc : { x : 1 } } );
assert.eq( "2,5" , s() , "B" );

t.update( { _id : 1 } , { $inc : { x : 1 } } );
assert.eq( "3,5" , s() , "C" );

t.update( { _id : 2 } , { $inc : { x : 1 } } );
assert.eq( "3,6" , s() , "D" );

t.update( {} , { $inc : { x : 1 } } , false , true );
assert.eq( "4,7" , s() , "E" );

t.update( {} , { $set : { x : 2 } } , false , true );
assert.eq( "2,2" , s() , "E" );

