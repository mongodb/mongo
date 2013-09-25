// An in order index is picked over a scan and order index when the in order index has sufficient
// matches to be picked, see discussion in SERVER-4150.

t = db.jstests_sorth;
t.drop();

t.ensureIndex( {a:1} );
t.ensureIndex( {b:1} );

function checkIndex( index, n ) {
    t.remove();
    for( i = 0; i < n; ++i ) {
        t.save( {a:i%2,b:i} );
    }
    explain = t.find( {a:0,b:{$gte:0}} ).sort( {b:1} ).explain( true );
    assert.eq( index, explain.cursor );
}

checkIndex( "BtreeCursor a_1", 100 );
checkIndex( "BtreeCursor b_1", 500 );
t.drop();