// Some unsatisfiable constraint tests.

t = db.jstests_queryoptimizer7;

function assertPlanWasRecorded( query ) {
    var x = t.find( query ).explain( true );
    assert.eq( 'BtreeCursor a_1', x.oldPlan.cursor , tojson(x) );
}

function assertNoPlanWasRecorded( query ) {
    assert( !t.find( query ).explain( true ).oldPlan );
}

// A query plan can be recorded and reused in the presence of a single key index
// constraint that would be impossible for a single key index, but is unindexed.
t.drop();
t.ensureIndex( {a:1} );
t.find( {a:1,b:1,c:{$gt:5,$lt:5}} ).itcount();
assertPlanWasRecorded( {a:1,b:1,c:{$gt:5,$lt:5}} );

// A query plan for an indexed unsatisfiable single key index constraint is not recorded.
t.drop();
t.ensureIndex( {a:1} );
t.find( {a:{$gt:5,$lt:5},b:1} ).itcount();
assertNoPlanWasRecorded( {a:{$gt:5,$lt:5},b:1} );

// A query plan for an unsatisfiable multikey index constraint is not recorded.
t.drop();
t.ensureIndex( {a:1} );
t.find( {a:{$in:[]},b:1} ).itcount();
assertNoPlanWasRecorded( {a:{$in:[]},b:1} );
