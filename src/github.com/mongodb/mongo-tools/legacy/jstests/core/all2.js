
t = db.all2;
t.drop();

t.save( { a : [ { x : 1 } , { x : 2  } ] } )
t.save( { a : [ { x : 2 } , { x : 3  } ] } )
t.save( { a : [ { x : 3 } , { x : 4  } ] } )

state = "no index";

function check( n , q , e ){
    assert.eq( n , t.find( q ).count() , tojson( q ) + " " + e + " count " + state );
    assert.eq( n , t.find( q ).itcount() , tojson( q ) + " " + e + " itcount" + state );
}

check( 1 , { "a.x" : { $in : [ 1 ] } } , "A" );
check( 2 , { "a.x" : { $in : [ 2 ] } } , "B" );

check( 2 , { "a.x" : { $in : [ 1 , 2 ] } } , "C" );
check( 3 , { "a.x" : { $in : [ 2 , 3 ] } } , "D" );
check( 3 , { "a.x" : { $in : [ 1 , 3 ] } } , "E" );

check( 1 , { "a.x" : { $all : [ 1 , 2 ] } } , "F" );
check( 1 , { "a.x" : { $all : [ 2 , 3 ] } } , "G" );
check( 0 , { "a.x" : { $all : [ 1 , 3 ] } } , "H" );

t.ensureIndex( { "a.x" : 1 } );
state = "index";

check( 1 , { "a.x" : { $in : [ 1 ] } } , "A" );
check( 2 , { "a.x" : { $in : [ 2 ] } } , "B" );

check( 2 , { "a.x" : { $in : [ 1 , 2 ] } } , "C" );
check( 3 , { "a.x" : { $in : [ 2 , 3 ] } } , "D" );
check( 3 , { "a.x" : { $in : [ 1 , 3 ] } } , "E" );

check( 1 , { "a.x" : { $all : [ 1 , 2 ] } } , "F" );
check( 1 , { "a.x" : { $all : [ 2 , 3 ] } } , "G" );
check( 0 , { "a.x" : { $all : [ 1 , 3 ] } } , "H" );

// --- more

t.drop();

t.save( { a : [ 1 , 2  ] } )
t.save( { a : [ 2 , 3  ] } )
t.save( { a : [ 3 , 4  ] } )

state = "more no index";

check( 1 , { "a" : { $in : [ 1 ] } } , "A" );
check( 2 , { "a" : { $in : [ 2 ] } } , "B" );

check( 2 , { "a" : { $in : [ 1 , 2 ] } } , "C" );
check( 3 , { "a" : { $in : [ 2 , 3 ] } } , "D" );
check( 3 , { "a" : { $in : [ 1 , 3 ] } } , "E" );

check( 1 , { "a" : { $all : [ 1 , 2 ] } } , "F" );
check( 1 , { "a" : { $all : [ 2 , 3 ] } } , "G" );
check( 0 , { "a" : { $all : [ 1 , 3 ] } } , "H" );

t.ensureIndex( { "a" : 1 } );
state = "more index";

check( 1 , { "a" : { $in : [ 1 ] } } , "A" );
check( 2 , { "a" : { $in : [ 2 ] } } , "B" );

check( 2 , { "a" : { $in : [ 1 , 2 ] } } , "C" );
check( 3 , { "a" : { $in : [ 2 , 3 ] } } , "D" );
check( 3 , { "a" : { $in : [ 1 , 3 ] } } , "E" );

check( 1 , { "a" : { $all : [ 1 , 2 ] } } , "F" );
check( 1 , { "a" : { $all : [ 2 , 3 ] } } , "G" );
check( 0 , { "a" : { $all : [ 1 , 3 ] } } , "H" );


// more 2

state = "more 2"

t.drop();
t.save( { name : [ "harry","jack","tom" ] } )
check( 0 , { name : { $all : ["harry","john"] } } , "A" );
t.ensureIndex( { name : 1 } );
check( 0 , { name : { $all : ["harry","john"] } } , "B" );

