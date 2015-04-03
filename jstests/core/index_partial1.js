// test simple index filters

function runTest() {
    t = db.index_filtered1;
    t.drop();

    t.ensureIndex( { x : 1 }, { filter : { a : { $lt : 5 } } } );

    for ( i = 0; i < 10; i++ ) {
        t.insert( { x : i, a : i } );
    }

    function getNumKeys() {
        var res = t.validate(true);
        return res.keysPerIndex[t.getFullName() + ".$x_1"];
    }

    assert.eq( 10, t.count() );
    assert.eq( 5, getNumKeys() );

    assert.eq( 10, t.find( { x : { $gte : 0 } } ).count() );
    assert.eq( 10, t.find( { x : { $gte : 0 } } ).itcount() );

    ex = t.find( { x : 2, a : { $lt : 5 } } ).explain(true);
    assert.eq( 1, ex.executionStats.nReturned );
    assert.eq( 1, ex.executionStats.totalDocsExamined );

    ex = t.find( { x : 2 } ).explain(true);
    assert.eq( 1, ex.executionStats.nReturned );
    assert.eq( 10, ex.executionStats.totalDocsExamined );

    ex = t.find( { x : 2, a : 2 } ).explain(true);
    assert.eq( 1, ex.executionStats.nReturned );
    assert.eq( 1, ex.executionStats.totalDocsExamined );

    t.dropIndex( { x : 1 } );
    assert.eq( 1, t.getIndexes().length );
    t.ensureIndex( { x : 1 }, { background : true, filter : { a : { $lt : 5 } } } );
    assert.eq( 5, getNumKeys() );

    t.dropIndex( { x : 1 } );
    assert.eq( 1, t.getIndexes().length );
    t.ensureIndex( { x : 1 }, { filter : { a : { $lt : 5 } } } );
    assert.eq( 5, getNumKeys() );

    t.dropIndex( { x : 1 } );
    assert.eq( 1, t.getIndexes().length );
    t.ensureIndex( { x : 1 } );
    assert.eq( 10, getNumKeys() );

    t.dropIndex( { x : 1 } );
    assert.eq( 1, t.getIndexes().length );

    // make sure I can't create invalid indexes

    assert.commandFailed( t.ensureIndex( { x : 1 }, { filter : 5 } ) );
    assert.commandFailed( t.ensureIndex( { x : 1 }, { filter : { x : { $asdasd : 3 } } } ) );

    assert.eq( 1, t.getIndexes().length );
}

if (db.serverStatus().process != "mongos") {
    runTest();
}
