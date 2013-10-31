// Index bounds within a singleton $or expression can be used with sorting.  SERVER-6416 SERVER-1205

t = db.jstests_orr;
t.drop();

function assertIndexBounds( bounds, query, sort, hint ) {
    cursor = t.find( query ).sort( sort );
    if ( hint ) {
        cursor.hint( hint );
    }
    assert.eq( bounds, cursor.explain().indexBounds.a );
}

t.ensureIndex( { a:1 } );

// Tight index bounds for a singleton $or expression with an indexed sort.
assertIndexBounds( [[ 1, 1 ]], { $or:[ { a:1 } ] }, { a:1 } );

// Tight index bounds for a nested singleton $or expression with an indexed sort.
assertIndexBounds( [[ 1, 1 ]], { $or:[ { $or:[ { a:1 } ] } ] }, { a:1 } );

// No index bounds computed for a non singleton $or expression with an indexed sort.
// QUERY_MIGRATION: a isn't multikey so we can get an indexed sort by unioning the two point ivals
//assertIndexBounds( [[ { $minElement:1 }, { $maxElement:1 } ]], { $or:[ { a:1 }, { a:2 } ] },
                   //{ a:1 } );

// Tight index bounds for a singleton $or expression with an unindexed sort.
assertIndexBounds( [[ 1, 1 ]], { $or:[ { a:1 } ] }, { b:1 } );

// No index bounds computed for a non singleton $or expression and an unindexed sort, so a $natural
// plan is used.
// QUERY_MIGRATION: we use an ixscan and then sort the results of that.
//assertIndexBounds( undefined, { $or:[ { a:1 }, { a:2 } ] }, { b:1 } );

// No index bounds computed for a non singleton $or expression with an unindexed sort.
// QUERY_MIGRATION: ditto
//assertIndexBounds( [[ { $minElement:1 }, { $maxElement:1 } ]], { $or:[ { a:1 }, { a:2 } ] },
                   //{ b:1 }, { a:1 } );
