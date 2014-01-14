// Some correctness checks for fast and normal count modes, including with skip and limit.

t = db.jstests_count6;

function checkCountForObject( obj ) {
    t.drop();
    t.ensureIndex( {b:1,a:1} );
    
    function checkCounts( query, expected ) {
        assert.eq( expected, t.count( query ) , "A1" );
        assert.eq( expected, t.find( query ).skip( 0 ).limit( 0 ).count( true ) , "A2" );
        // Check proper counts with various skip and limit specs.
        for( var skip = 1; skip <= 2; ++skip ) {
            for( var limit = 1; limit <= 2; ++limit ) {
                assert.eq( Math.max( expected - skip, 0 ), t.find( query ).skip( skip ).count( true ) , "B1" );
                assert.eq( Math.min( expected, limit ), t.find( query ).limit( limit ).count( true ) , "B2" );
                assert.eq( Math.min( Math.max( expected - skip, 0 ), limit ), t.find( query ).skip( skip ).limit( limit ).count( true ) , "B4" );

                // Check limit(x) = limit(-x)
                assert.eq( t.find( query ).limit( limit ).count( true ),
                           t.find( query ).limit( -limit ).count( true ) , "C1" );
                assert.eq( t.find( query ).skip( skip ).limit( limit ).count( true ),
                           t.find( query ).skip( skip ).limit( -limit ).count( true ) , "C2" );
            }
        }

        // Check limit(0) has no effect
        assert.eq( expected, t.find( query ).limit( 0 ).count( true ) , "D1" );
        assert.eq( Math.max( expected - skip, 0 ),
                   t.find( query ).skip( skip ).limit( 0 ).count( true ) , "D2" );
        assert.eq( expected, t.getDB().runCommand({ count: t.getName(),
                        query: query, limit: 0 }).n , "D3" );
        assert.eq( Math.max( expected - skip, 0 ),
                   t.getDB().runCommand({ count: t.getName(),
                               query: query, limit: 0, skip: skip }).n , "D4" );
    }

    for( var i = 0; i < 5; ++i ) {
        checkCounts( {a:obj.a,b:obj.b}, i );
        checkCounts( {b:obj.b,a:obj.a}, i );
        t.insert( obj );    
    }

    t.insert( {a:true,b:true} );
    t.insert( {a:true,b:1} );
    t.insert( {a:false,b:1} );
    t.insert( {a:false,b:true} );
    t.insert( {a:false,b:false} );

    checkCounts( {a:obj.a,b:obj.b}, i );
    checkCounts( {b:obj.b,a:obj.a}, i );

    // Check with no query
    checkCounts( {}, 10 );
}

// Check fast count mode.
checkCountForObject( {a:true,b:false} );

// Check normal count mode.
checkCountForObject( {a:1,b:0} );
