
t = db.distinct_index1
t.drop();

function r(){
    return Math.floor( Math.random() * 10 );
}

function d( k , q ){
    return t.runCommand( "distinct" , { key : k , query : q || {} } )
}

for ( i=0; i<1000; i++ ){
    o = { a : r() , b : r() };
    t.insert( o );
}

x = d( "a" );
assert.eq( 1000 , x.stats.n , "A1" )
assert.eq( 1000 , x.stats.nscanned , "A2" )

x = d( "a" , { a : { $gt : 5 } } );
assert.eq( 1000 , x.stats.n , "A1" )
assert.eq( 1000 , x.stats.nscanned , "A2" )




