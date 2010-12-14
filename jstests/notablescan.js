// check notablescan mode

t = db.test_notablescan;
t.drop();

try {
    assert.commandWorked( db._adminCommand( { setParameter:1, notablescan:true } ) );
    // commented lines are SERVER-2222
//    assert.throws( function() { t.find( {a:1} ).toArray(); } );
    t.save( {a:1} );
//    assert.throws( function() { t.count( {a:1} ); } );
//    assert.throws( function() { t.find( {} ).toArray(); } );
    assert.throws( function() { t.find( {a:1} ).toArray(); } );
    assert.throws( function() { t.find( {a:1} ).hint( {$natural:1} ).toArray(); } );
    t.ensureIndex( {a:1} );
    assert.eq( 0, t.find( {a:1,b:1} ).itcount() );    
    assert.eq( 1, t.find( {a:1,b:null} ).itcount() );    
} finally {
    // We assume notablescan was false before this test started and restore that
    // expected value.
    assert.commandWorked( db._adminCommand( { setParameter:1, notablescan:false } ) );
}
