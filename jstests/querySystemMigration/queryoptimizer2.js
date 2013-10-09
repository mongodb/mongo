
t = db.queryoptimizer2;

function debug( x ) {
//    printjson( x );
}

function doTest( f1, f2 ) {

    t.drop()
        
    for( i = 0; i < 30; ++i ) {
        t.save( { a:2, z:1 } );
    }

    for( i = 0; i < 30; ++i ) {
        t.save( { b:2, z:2 } );
    }

    for( i = 0; i < 60; ++i ) {
        t.save( { c:2, z:3 } );
    }

    t.ensureIndex( { a:1 } );
    t.ensureIndex( { b:1 } );
    t.ensureIndex( { z:1 } );

    e = t.find( { b:2, z:2 } ).batchSize( 100 ).explain( true );
    assert.eq( null, e.oldPlan );

    t.ensureIndex( { c:1 } ); // will clear query cache

    f1();

    assert( t.find( { a:2, z:1 } ).batchSize( 100 ).explain( true ).oldPlan );
    assert( t.find( { b:2, z:2 } ).batchSize( 100 ).explain( true ).oldPlan );

    e = t.find( { c:2, z:3 } ).batchSize( 100 ).explain( true );
    // no pattern should be recorded as a result of the $or query
    assert.eq( null, e.oldPlan );

    t.dropIndex( { b:1, z:2 } ); // clear query cache
    for( i = 0; i < 15; ++i ) {
        t.save( { a:2, z:1 } );
    }

    f2();
    // pattern should be recorded, since we transitioned to the takeover cursor and had over 50 matches
    assert( t.find( { c:2, z:3 } ).batchSize( 100 ).explain( true ).oldPlan );

}

doTest( function() {
       t.find( { $or: [ { a:2, z:1 }, { b:2, z:2 }, { c:2, z:3 } ] } ).batchSize( 100 ).toArray();
       },
       function() {
       t.find( { $or: [ { a:2, z:1 }, { c:2, z:3 } ] } ).batchSize( 100 ).toArray();       
       }
       );

doTest( function() {
       t.find( { $or: [ { a:2, z:1 }, { b:2, z:2 }, { c:2, z:3 } ] } ).limit(102).count( true );
       },
       function() {
       t.find( { $or: [ { a:2, z:1 }, { c:2, z:3 } ] } ).limit(102).count( true );       
       }
       );
