// Test simple regex optimization with a regex | (bar) present - SERVER-3298

t = db.jstests_regexa;
t.drop();

function check() {
    assert.eq( 1, t.count( {a:/^(z|.)/} ) );
    assert.eq( 1, t.count( {a:/^z|./} ) );
    assert.eq( 0, t.count( {a:/^z(z|.)/} ) );    
    assert.eq( 1, t.count( {a:/^zz|./} ) );    
}

t.save( {a:'a'} );

check();
t.ensureIndex( {a:1} );
if ( 1 ) { // SERVER-3298
check();
}
