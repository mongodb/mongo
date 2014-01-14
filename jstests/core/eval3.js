
t = db.eval3;
t.drop();

t.save( { _id : 1 , name : "eliot" } );
assert.eq( 1 , t.count() , "A" );

function z( a , b ){
    db.eval3.save( { _id : a , name : b } );
    return b;
}

z( 2 , "sara" );
assert.eq( 2 , t.count() , "B" );

assert.eq( "eliot,sara" , t.find().toArray().map( function(z){ return z.name; } ).sort().toString() );

assert.eq( "joe" , db.eval( z , 3 , "joe" ) , "C" );
assert.eq( 3 , t.count() , "D" );

assert.eq( "eliot,joe,sara" , t.find().toArray().map( function(z){ return z.name; } ).sort().toString() );
