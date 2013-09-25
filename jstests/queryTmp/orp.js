// Delete and update work properly when the document to be modified is the first of a new unindexed
// $or clause.  SERVER-5198

t = db.jstests_orp;
t.drop();

function verifyExpectedQueryPlan( query ) {
    explain = t.find( query ).explain();
    assert.eq( 'BtreeCursor a_1', explain.clauses[ 0 ].cursor );
    assert.eq( 'BasicCursor', explain.clauses[ 1 ].cursor );
}

function checkAdvanceWithWriteOp( writeOp ) {
    t.drop();
   
    for( i = 0; i < 120; ++i ) {
        t.insert( { a:119-i, b:2 } );
    }

    t.ensureIndex( { a:1 } );

    // The presence of an index on the b field causes the query below to generate query plans for
    // each $or clause iteratively rather than run a simple unindexed query plan.
    t.ensureIndex( { c:1, b:1 } );

    // The cursors traversed for this $or query will be a:1, $natural:1.  The second clause will run
    // as an unindexed scan because no index has b as its first field.  The a:119 document will
    // be the last document of the first clause and the first document of the (unindexed) second
    // clause.
    query = { $or:[ { a:{ $gte:0 } }, { b:2 } ] };
    verifyExpectedQueryPlan( query );
    writeOp( query );
    assert( !db.getLastError() );
    assert.eq( 120, db.getLastErrorObj().n );
}

// Remove.
checkAdvanceWithWriteOp( function( query ) { t.remove( query ); } );
// The documents were removed.
assert.eq( 0, t.count() );
assert.eq( 0, t.find().itcount() );

// Update - add a large field so the document will move.
big = new Array( 10000 ).toString();
checkAdvanceWithWriteOp( function( query ) { t.update( query, { $push:{ z:big } }, false,
                                                      true ); } );
// The documents were updated.
assert.eq( 120, t.count( { z:{ $size:1 } } ) );
