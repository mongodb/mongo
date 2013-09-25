// SERVER-393 Test indexed matching with $exists.

t = db.jstests_exists6;
t.drop();

t.ensureIndex( {b:1} );
t.save( {} );
t.save( {b:1} );
t.save( {b:null} );

checkExists = function( query ) {
    // Index range constraint on 'b' is universal, so a BasicCursor is the default cursor type.
    var x = t.find( query ).explain()
    /* NEW QUERY EXPLAIN
    assert.eq( 'BasicCursor', x.cursor , tojson(x) );
    */
    // Index bounds include all elements.
    
    var x = t.find( query ).hint( {b:1} ).explain()
    /* NEW QUERY EXPLAIN
    if ( ! x.indexBounds ) x.indexBounds = {}
    */
    /* NEW QUERY EXPLAIN
    assert.eq( [ [ { $minElement:1 }, { $maxElement:1 } ] ], x.indexBounds.b , tojson(x) );
    */
    // All keys must be scanned.
    t.find( query ).hint( {b:1} );
    // 2 docs will match.
    assert.eq( 2, t.find( query ).hint( {b:1} ).itcount() );    
}
checkExists( {b:{$exists:true}} );
checkExists( {b:{$not:{$exists:false}}} );

checkMissing = function( query ) {
    // Index range constraint on 'b' is not universal, so a BtreeCursor is the default cursor type.
    t.find( query ).explain();
    // Scan null index keys.
    // NEW QUERY EXPLAIN
    t.find( query ).explain();
    // Two existing null keys will be scanned.
    // NEW QUERY EXPLAIN
    t.find( query ).explain();
    // One doc is missing 'b'.
    assert.eq( 1, t.find( query ).hint( {b:1} ).itcount() );    
}
checkMissing( {b:{$exists:false}} );
checkMissing( {b:{$not:{$exists:true}}} );

// Now check existence of second compound field.
t.ensureIndex( {a:1,b:1} );
t.save( {a:1} );
t.save( {a:1,b:1} );
t.save( {a:1,b:null} );

checkExists = function( query ) {
    // Index bounds include all elements.
    // NEW QUERY EXPLAIN
    t.find( query ).explain();
    // All keys must be scanned.
    // NEW QUERY EXPLAIN
    t.find( query ).explain();
    // 2 docs will match.
    assert.eq( 2, t.find( query ).hint( {a:1,b:1} ).itcount() );    
}
checkExists( {a:1,b:{$exists:true}} );
checkExists( {a:1,b:{$not:{$exists:false}}} );

checkMissing = function( query ) {
    // Scan null index keys.
    // NEW QUERY EXPLAIN
    t.find( query ).explain();
    // Two existing null keys will be scanned.
    // NEW QUERY EXPLAIN
    t.find( query ).explain();
    // One doc is missing 'b'.
    assert.eq( 1, t.find( query ).hint( {a:1,b:1} ).itcount() );    
}
checkMissing( {a:1,b:{$exists:false}} );
checkMissing( {a:1,b:{$not:{$exists:true}}} );
