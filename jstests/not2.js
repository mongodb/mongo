t = db.jstests_not2;
t.drop();

check = function( query, expected, size ) {
    if ( size == null ) {
        size = 1;
    }
    assert.eq( size, t.count( query ), tojson( query ) );
    if ( size > 0 ) {
        assert.eq( expected, t.findOne( query ).i, tojson( query ) );
    }
}

fail = function( query ) {
    t.count( query );
    assert( db.getLastError(), tojson( query ) );
}

t.save( {i:"a"} );
t.save( {i:"b"} );

fail( {i:{$not:"a"}} );
check( {i:{$not:{$gt:"a"}}}, "a" );
check( {i:{$not:{$gte:"b"}}}, "a" );
check( {i:{$exists:true}}, "a", 2 );
check( {i:{$not:{$exists:true}}}, "", 0 );
check( {j:{$not:{$exists:false}}}, "", 0 );
check( {j:{$not:{$exists:true}}}, "a", 2 );
check( {i:{$not:{$in:["a"]}}}, "b" );
check( {i:{$not:{$in:["a", "b"]}}}, "", 0 );
check( {i:{$not:{$in:["g"]}}}, "a", 2 );
check( {i:{$not:{$nin:["a"]}}}, "a" );
// $mod
// $size
check( {i:{$not:/a/}}, "b" );
check( {i:{$not:/(a|b)/}}, "", 0 );
check( {i:{$not:/a/,$regex:"a"}}, "", 0 );
check( {i:{$not:/aa/}}, "a", 2 );
fail( {i:{$not:{$regex:"a"}}} );
fail( {i:{$not:{$options:"a"}}} );

t.drop();
t.save( {i:1} );
check( {i:{$not:{$mod:[5,1]}}}, null, 0 );
check( {i:{$mod:[5,2]}}, null, 0 );
check( {i:{$not:{$mod:[5,2]}}}, 1, 1 );

//in array
//$all
//$elemMatch

// check index usage

