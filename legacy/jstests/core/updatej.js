// Test that update validation failure terminates the update without modifying subsequent
// documents.  SERVER-4779

t = db.jstests_updatej;
t.drop();

t.save( {a:[]} );
t.save( {a:1} );
t.save( {a:[]} );

t.update( {}, {$push:{a:2}}, false, true );
assert.eq( 1, t.count( {a:2} ) );
