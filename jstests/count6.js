// Some correctness checks for fast and normal count modes, including with skip and limit.

t = db.jstests_count6;

function checkCountForObject( obj ) {
    t.drop();
    t.ensureIndex( {b:1,a:1} );
    
    function checkCounts( query, expected ) {
        assert.eq( expected, t.count( query ) );
        assert.eq( expected, t.find( query ).skip( 0 ).limit( 0 ).count( true ) );
        // Check proper counts with various skip and limit specs.
        for( var skip = 1; skip <= 2; ++skip ) {
            for( var limit = 1; limit <= 2; ++limit ) {
                assert.eq( Math.max( expected - skip, 0 ), t.find( query ).skip( skip ).count( true ) );
                assert.eq( Math.min( expected, limit ), t.find( query ).limit( limit ).count( true ) );
                assert.eq( Math.min( Math.max( expected - skip, 0 ), limit ), t.find( query ).skip( skip ).limit( limit ).count( true ) );            
            }
        }
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
}

// Check fast count mode.
checkCountForObject( {a:true,b:false} );

// Check normal count mode.
checkCountForObject( {a:1,b:0} );
