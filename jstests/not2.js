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

t.save( {i:"a"} );
t.save( {i:"b"} );

//check( {i:{$not:"a"}}, "b" );
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
check( {i:{$not:/a/,$regex:"b"}}, "b" );
check( {i:{$not:/aa/}}, "a", 2 );
// other type of regex

//in array
//$all
//$elemMatch

// check index usage

