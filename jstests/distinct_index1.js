
t = db.distinct_index1
t.drop();

function r( x ){
    return Math.floor( Math.sqrt( x * 123123 ) ) % 10;
}

function d( k , q ){
    return t.runCommand( "distinct" , { key : k , query : q || {} } )
}

for ( i=0; i<1000; i++ ){
    o = { a : r(i*5) , b : r(i) };
    t.insert( o );
}

x = d( "a" );
assert.eq( 1000 , x.stats.n , "A1" )
assert.eq( 1000 , x.stats.nscanned , "A2" )
assert.eq( 1000 , x.stats.nscannedObjects , "A3" )

x = d( "a" , { a : { $gt : 5 } } );
assert.eq( 398 , x.stats.n , "B1" )
assert.eq( 1000 , x.stats.nscanned , "B2" )
assert.eq( 1000 , x.stats.nscannedObjects , "B3" )

t.ensureIndex( { a : 1 } )

x = d( "a" );
assert.eq( 1000 , x.stats.n , "C1" )
assert.eq( 1000 , x.stats.nscanned , "C2" )
assert.eq( 1000 , x.stats.nscannedObjects , "C3" )

x = d( "a" , { a : { $gt : 5 } } );
assert.eq( 398 , x.stats.n , "D1" )
assert.eq( 398 , x.stats.nscanned , "D2" )
assert.eq( 0 , x.stats.nscannedObjects , "D3" )





