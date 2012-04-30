// Delete and update work properly when the document to be modified is the first of a new unindexed
// $or clause.  SERVER-5198

t = db.jstests_orp;
t.drop();

function checkAdvanceWithWriteOp( writeOp ) {
    t.drop();
   
    for( i = 0; i < 120; ++i ) {
        t.insert( { a:119-i, b:2 } );
    }

    t.ensureIndex( { a:1 } );
    t.ensureIndex( { a:1, b:1 } );

    // The cursors traversed for this $or query will be a:1, $natural:1.  The a:119 document will
    // be the last document of the first clause and the first document of the (unindexed) second
    // clause.
    writeOp( { $or:[ { a:{ $gte:0 } }, { b:2 } ] } );
    assert( !db.getLastError() );
}

// Remove.
checkAdvanceWithWriteOp( function( query ) { t.remove( query ); } );

// Update - add a large field so the document will move.
big = new Array( 10000 ).toString();
checkAdvanceWithWriteOp( function( query ) { t.update( query, { $set:{ z:big } }, false,
                                                      true ); } );
