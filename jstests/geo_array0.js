// Make sure the very basics of geo arrays are sane by creating a few multi location docs
t = db.geoarray

function test(index) {
    t.drop();
    t.insert( { zip : "10001", loc : { home : [ 10, 10 ], work : [ 50, 50 ] } } )
    t.insert( { zip : "10002", loc : { home : [ 20, 20 ], work : [ 50, 50 ] } } )
    t.insert( { zip : "10003", loc : { home : [ 30, 30 ], work : [ 50, 50 ] } } )
    assert.isnull( db.getLastError() )

    if (index) {
        t.ensureIndex( { loc : "2d", zip : 1 } );
        assert.isnull( db.getLastError() )
        assert.eq( 2, t.getIndexKeys().length )
    }

    t.insert( { zip : "10004", loc : { home : [ 40, 40 ], work : [ 50, 50 ] } } )
    assert.isnull( db.getLastError() )

    // test normal access
    printjson( t.find( { loc : { $within : { $box : [ [ 0, 0 ], [ 45, 45 ] ] } } } ).toArray() )
    assert.eq( 4, t.find( { loc : { $within : { $box : [ [ 0, 0 ], [ 45, 45 ] ] } } } ).count() );
    assert.eq( 4, t.find( { loc : { $within : { $box : [ [ 45, 45 ], [ 50, 50 ] ] } } } ).count() );
}

//test(false); // this was removed as part of SERVER-6400
test(true)
