// Test sorting with long longs and doubles - SERVER-3719

t = db.jstests_numberlong3;
t.drop();

s = "11235399833116571";
for( i = 10; i >= 0; --i ) {
    n = NumberLong( s + i );
    t.save( {x:n} );
    if ( 0 ) { // SERVER-3719
    t.save( {x:n.floatApprox} );
    }
}

ret = t.find().sort({x:1}).toArray().filter( function( x ) { return typeof( x.x.floatApprox ) != 'undefined' } );

//printjson( ret );

for( i = 1; i < ret.length; ++i ) {
    first = ret[i-1].x.toString();
    second = ret[i].x.toString();
    if ( first.length == second.length ) {
        assert.lte( ret[i-1].x.toString(), ret[i].x.toString() );
    }
}
