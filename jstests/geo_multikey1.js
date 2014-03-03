// Multikey geo index tests with parallel arrays.

t = db.jstests_geo_multikey1;
t.drop();

locArr = [];
arr = [];
for( i = 0; i < 10; ++i ) {
    locArr.push( [i,i+1] );
    arr.push( i );
}
t.save( {loc:locArr,a:arr,b:arr,c:arr} );

// Parallel arrays are allowed for geo indexes.
t.ensureIndex( {loc:'2d',a:1,b:1,c:1} );
assert( !db.getLastError() );

// Parallel arrays are not allowed for normal indexes.
t.ensureIndex( {loc:1,a:1,b:1,c:1} );
assert( db.getLastError() );
