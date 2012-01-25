// Test that update validation failure doesn't terminate the update without modifying subsequent
// documents.  SERVER-4779

t = db.jstests_updatej;
t.drop();

t.save( {a:[]} );
t.save( {a:1} );
t.save( {a:[]} );

t.update( {}, {$push:{a:2}}, false, true );
if ( 0 ) { // SERVER-4779
assert.eq( 2, t.count( {a:2} ) );
}
