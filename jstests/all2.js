
t = db.all2;
t.drop();

t.save( { a : [ { x : 1 } , { x : 2  } ] } )
t.save( { a : [ { x : 2 } , { x : 3  } ] } )
t.save( { a : [ { x : 3 } , { x : 4  } ] } )

function check( n , q , e ){
    assert.eq( n , t.find( q ).count() , tojson( q ) + " " + e );
}

check( 1 , { "a.x" : { $in : [ 1 ] } } , "A" );
check( 2 , { "a.x" : { $in : [ 2 ] } } , "B" );

check( 2 , { "a.x" : { $in : [ 1 , 2 ] } } , "C" );
check( 3 , { "a.x" : { $in : [ 2 , 3 ] } } , "D" );
check( 3 , { "a.x" : { $in : [ 1 , 3 ] } } , "E" );

check( 1 , { "a.x" : { $all : [ 1 , 2 ] } } , "F" );
check( 1 , { "a.x" : { $all : [ 2 , 3 ] } } , "G" );
check( 0 , { "a.x" : { $all : [ 1 , 3 ] } } , "H" );
