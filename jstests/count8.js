// Drop a collection while a count operation is yielding, in both fast and normal count modes.

t = db.jstests_count8;
t.drop();

function assertCollectionMissing( query ) {
    assert.commandFailed( t.stats() );
    assert.eq( 0, t.count( query ) );    
}

function checkDrop( fastCount, query ) {

    obj = fastCount ? { a:true } : { a:1 };
    query = query || obj;
    
    nDocs = 40000;

    t.drop();
    t.ensureIndex( { a:1 } );
    for( i = 0; i < nDocs; ++i ) {
        t.insert( obj );
    }
    db.getLastError();

    // Trigger an imminent collection drop.  If the drop operation runs while the count operation
    // yields, count returns a valid result.  Additionally mongod continues running and indicates
    // the collection is no longer present.  The same will be true if, in an alternative timing
    // scenario, the drop operation runs before count starts or after count finishes. 
    p = startParallelShell( 'sleep( 15 ); db.jstests_count8.drop(); db.getLastError();' );

    // The count command runs successfully so count() does not assert.
    t.count( query );
    p();

    assertCollectionMissing( query );
}

checkDrop( true );
checkDrop( false );
checkDrop( false, {$or:[{a:1},{a:2}]} );
