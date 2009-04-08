// Test that client gets stack trace on failed invoke

f = db.jstests_error2;

f.drop();

f.save( {a:1} );

c = f.find({$where : function(){ return a() }});
try {
    c.next();
} catch( e ) {
    assert( e.match( /java.lang.NullPointerException/ ) );
}

try {
    db.eval( function() { return a(); } );
} catch ( e ) {
    assert( e.match( /java.lang.NullPointerException/ ) );
}
