// Tests whether the geospatial search is stable under btree updates

var coll = db.getCollection( "jstests_geo_update_btree" )
coll.drop()

coll.ensureIndex( { loc : '2d' } )

for ( i = 0; i < 10000; i++ ) {
    coll.insert( { loc : [ Random.rand() * 180, Random.rand() * 180 ], v : '' } );
}

var big = new Array( 3000 ).toString()

for ( i = 0; i < 1000; i++ ) {
    coll.update(
            { loc : { $within : { $center : [ [ Random.rand() * 180, Random.rand() * 180 ], Random.rand() * 50 ] } } },
            { $set : { v : big } }, false, true )

    if (testingReplication)
        db.getLastError(2);
    else
        db.getLastError();

    if( i % 10 == 0 ) print( i );
}
