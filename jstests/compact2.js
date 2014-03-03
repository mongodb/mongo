// Compaction of a v0 index converts it to a v1 index using a v1 index comparator during external
// sort.  SERVER-6499

t = db.jstests_compact2;
t.drop();

/**
 * Assert that the index is of the expected version and its keys are ordered consistently with this
 * version, and that the unique and background fields are set correctly.
 */
function assertIndex( expectedVersion, unique, background ) {
    indexSpec = db.system.indexes.findOne( { ns:t.toString(), key:{ date:1 } } );
    // The index version is as expected.
    assert.eq( expectedVersion, indexSpec.v );
    // The index uniqueness is as expected (treat missing and false unique specs as equivalent).
    assert.eq( !unique, !indexSpec.unique );
    // Background is as expected.
    assert.eq( !background, !indexSpec.background );
    // Check that 'date' key ordering is consistent with the index version.
    dates = t.find().hint( { date:1 } ).toArray().map( function( x ) { return x.date; } );
    if ( expectedVersion == 0 ) {
        // Under v0 index comparison, new Date( -1 ) > new Date( 1 ).
        assert.eq( [ new Date( 1 ), new Date( -1 ) ], dates );
    }
    else {
        // Under v1 index comparsion, new Date( -1 ) < new Date( 1 ).
        assert.eq( [ new Date( -1 ), new Date( 1 ) ], dates );
    }
}

/** Compact a collection and check the resulting indexes. */
function checkCompact( originalVersion, unique, background ) {
    t.drop();
    t.save( { date:new Date( 1 ) } );
    t.save( { date:new Date( -1 ) } );
    t.ensureIndex( { date:1 }, { unique:unique, v:originalVersion, background:background } );
    assertIndex( originalVersion, unique, background );

    // Under SERVER-6499, compact fails when a v0 index is converted to a v1 index and key
    // comparisons are inconsistent, as with the date values in this test.
    assert.commandWorked( t.runCommand( "compact" ) );
    assert( !db.getLastError() );

    // Compact built an index with the default index version (v1).  Uniqueness is maintained, but
    // background always becomes false.
    assertIndex( 1, unique, false );
}

checkCompact( 0, true, true );
checkCompact( 0, false, false );
checkCompact( 1, true, false );
checkCompact( 1, false, true );
