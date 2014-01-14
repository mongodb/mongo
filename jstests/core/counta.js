// Check that count returns 0 in some exception cases.

t = db.jstests_counta;
t.drop();

for( i = 0; i < 10; ++i ) {
    t.save( {a:i} );
}

// f() is undefined, causing an assertion 
assert.throws( 
    function(){ 
        t.count( { $where:function() { if ( this.a < 5 ) { return true; } else { f(); } } } );
    } );
