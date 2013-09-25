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
assertIndexBounds( [[ { $minElement:1 }, { $maxElement:1 } ]], { $or:[ { a:1 }, { a:2 } ] },
                   { a:1 } );

// Tight index bounds for a singleton $or expression with an unindexed sort.
assertIndexBounds( [[ 1, 1 ]], { $or:[ { a:1 } ] }, { b:1 } );

// No index bounds computed for a non singleton $or expression and an unindexed sort, so a $natural
// plan is used.
assertIndexBounds( undefined, { $or:[ { a:1 }, { a:2 } ] }, { b:1 } );

// No index bounds computed for a non singleton $or expression with an unindexed sort.
assertIndexBounds( [[ { $minElement:1 }, { $maxElement:1 } ]], { $or:[ { a:1 }, { a:2 } ] },
                   { b:1 }, { a:1 } );
