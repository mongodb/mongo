// SERVER-393 Test indexed matching with $exists.

t = db.jstests_exists6;
t.drop();

t.ensureIndex( {b:1} );
t.save( {} );
t.save( {b:1} );
t.save( {b:null} );

//---------------------------------

function checkIndexUse( query, usesIndex, index, bounds ) {
    var x = t.find( query ).explain()
    if ( usesIndex ) {
        assert.eq( x.cursor.indexOf(index), 0 , tojson(x) );
        if ( ! x.indexBounds ) x.indexBounds = {}
        assert.eq( bounds, x.indexBounds.b , tojson(x) );
    }
    else {
        assert.eq( 'BasicCursor', x.cursor, tojson(x) );
    }
}

function checkExists( query, usesIndex, bounds ) {
    checkIndexUse( query, usesIndex, 'BtreeCursor b_1', bounds );
    // Whether we use an index or not, we will always scan all docs.
    assert.eq( 3, t.find( query ).explain().nscanned );
    // 2 docs will match.
    assert.eq( 2, t.find( query ).itcount() );
}

function checkMissing( query, usesIndex, bounds ) {
    checkIndexUse( query, usesIndex, 'BtreeCursor b_1', bounds );
    // Nscanned changes based on index usage.
    if ( usesIndex ) assert.eq( 2, t.find( query ).explain().nscanned );
    else assert.eq( 3, t.find( query ).explain().nscanned );
    // 1 doc is missing 'b'.
    assert.eq( 1, t.find( query ).itcount() );
}

function checkExistsCompound( query, usesIndex, bounds ) {
    checkIndexUse( query, usesIndex, 'BtreeCursor', bounds );
    if ( usesIndex ) assert.eq( 3, t.find( query ).explain().nscanned );
    else assert.eq( 3, t.find( query ).explain().nscanned );
    // 2 docs have a:1 and b:exists.
    assert.eq( 2, t.find( query ).itcount() );
}

function checkMissingCompound( query, usesIndex, bounds ) {
    checkIndexUse( query, usesIndex, 'BtreeCursor', bounds );
    // two possible indexes to use
    // 1 doc should match
    assert.eq( 1, t.find( query ).itcount() );
}

//---------------------------------

var allValues = [ [ { $minElement:1 }, { $maxElement:1 } ] ];
var nullNull = [ [ null, null ] ];

// Basic cases
checkExists( {b:{$exists:true}}, true, allValues );
// We change this to not -> not -> exists:true, and get allValue for bounds
// but we use a BasicCursor?
checkExists( {b:{$not:{$exists:false}}}, false, allValues );
checkMissing( {b:{$exists:false}}, true, nullNull );
checkMissing( {b:{$not:{$exists:true}}}, true, nullNull );

// Now check existence of second compound field.
t.ensureIndex( {a:1,b:1} );
t.save( {a:1} );
t.save( {a:1,b:1} );
t.save( {a:1,b:null} );

checkExistsCompound( {a:1,b:{$exists:true}}, true, allValues );
checkExistsCompound( {a:1,b:{$not:{$exists:false}}}, true, allValues );
checkMissingCompound( {a:1,b:{$exists:false}}, true, nullNull );
checkMissingCompound( {a:1,b:{$not:{$exists:true}}}, true, nullNull );
