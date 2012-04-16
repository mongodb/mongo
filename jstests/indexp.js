// Check recording and playback of good query plans with different index types SERVER-958.

t = db.jstests_indexp;
t.drop();

function expectRecordedPlan( query, idx ) {
    explain = t.find( query ).explain( true );
    assert( explain.oldPlan );
 	assert.eq( "BtreeCursor " + idx, explain.oldPlan.cursor );
}

function expectNoRecordedPlan( query ) {
 	assert.isnull( t.find( query ).explain( true ).oldPlan );
}

// Basic test
t.drop();
t.ensureIndex( {a:1} );
t.save( {a:1} );
t.find( {a:1,x:1} ).itcount();
expectRecordedPlan( {a:1,x:1}, "a_1" );

// Index type changes
t.drop();
t.ensureIndex( {a:1} );
t.save( {a:1} );
t.find( {a:1,x:1} ).itcount();
t.save( {a:[1,2]} );
expectNoRecordedPlan( {a:1,x:1} );

// Multi key QueryPattern reuses index
t.drop();
t.ensureIndex( {a:1} );
t.ensureIndex( {x:1} );
t.save( {a:[1,2],x:1} );
for( i = 0; i < 5; ++i ) {
    t.save( {a:-1,x:i} );
}
t.find( {a:{$gt:0},x:{$gt:0}} ).itcount();
expectRecordedPlan( {a:{$gt:0,$lt:5},x:{$gt:0}}, "a_1" );

// Single key QueryPattern is dropped.
t.drop();
t.ensureIndex( {a:1} );
t.save( {a:1} );
t.find( {a:{$gt:0,$lt:5},x:1} ).itcount();
t.save( {a:[1,2]} );
expectNoRecordedPlan( {a:{$gt:0,$lt:5},x:1} );

// Invalid query with only valid fields used 
// SERVER-2864
t.drop();
t.ensureIndex( {a:1} );
t.save( {a:1}  );
t.find( {a:1,b:{$gt:5,$lt:0},x:1} ).itcount();
expectRecordedPlan( {a:1,b:{$gt:5,$lt:0},x:1}, "a_1" );

// SERVER-2864
t.drop();
t.ensureIndex( {a:1} );
t.ensureIndex( {c:1} );
t.save( {a:1,c:1} );
t.save( {c:1} );
t.find( {a:1,b:{$gt:5,$lt:0},c:1} ).itcount();
expectRecordedPlan( {a:1,b:{$gt:5,$lt:0},c:1}, "a_1" );

// SERVER-2864
t.drop();
t.ensureIndex( {a:1} );
t.save( {a:1} );
t.find( {a:{$gt:5,$lt:0},x:1} ).itcount();
expectNoRecordedPlan( {a:{$gt:0,$lt:5},x:1}, "a_1" );

// Dummy query plan not stored
t.drop();
t.ensureIndex( {a:1} );
t.save( {a:1} );
t.find( {a:{$gt:5,$lt:0},x:1} ).itcount();
expectNoRecordedPlan( {a:{$gt:5,$lt:0},x:1} );
