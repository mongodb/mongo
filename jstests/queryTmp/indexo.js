// Check that dummy basic cursors work correctly SERVER-958.

t = db.jstests_indexo;
t.drop();

function checkDummyCursor( explain ) {
 	assert.eq( "BasicCursor", explain.cursor );
    assert.eq( 0, explain.nscanned );
    assert.eq( 0, explain.n );
}

t.save( {a:1} );

t.ensureIndex( {a:1} );

// Match is impossible, so no documents should be scanned.
checkDummyCursor( t.find( {a:{$gt:5,$lt:0}} ).explain() );

t.drop();
checkDummyCursor( t.find( {a:1} ).explain() );

t.save( {a:1} );
t.ensureIndex( {a:1} );
checkDummyCursor( t.find( {$or:[{a:{$gt:5,$lt:0}},{a:1}]} ).explain().clauses[ 0 ] );

t.drop();
t.save( {a:5,b:[1,2]} );
t.ensureIndex( {a:1,b:1} );
t.ensureIndex( {a:1} );
// The first clause will use index {a:1,b:1} with the current implementation.
// The second clause has no valid values for index {a:1} so it will use a dummy cursor.
checkDummyCursor( t.find( {$or:[{b:{$exists:true},a:{$gt:4}},{a:{$lt:6,$gt:4}}]} ).explain().clauses[ 1 ] );
