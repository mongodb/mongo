// Check that optimal query plans do not contain empty but unindexed ranges.

t = db.jstests_indexz;
t.drop();

t.ensureIndex( {a:1} );
t.ensureIndex( {a:1,b:1} );

t.save( {a:1,b:[1,3]} );

// The plan for single key index {a:1} is not marked optimal because a constraint exists on field
// {b:1}.
explain = t.find( {a:1,b:{$gt:2,$lt:2}} ).explain(true);
assert.eq( 'BtreeCursor a_1_b_1', explain.cursor );
assert.eq( 1, explain.allPlans.length );

