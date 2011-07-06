// Check nonoverlapping $in/$all with multikeys SERVER-2165

t = db.jstests_indexl;
t.drop();

t.save( {a:[1,2]} );
assert.eq( 1, t.count( {a:{$all:[1],$in:[2]}} ) );
assert.eq( 1, t.count( {a:{$all:[2],$in:[1]}} ) );
assert.eq( 1, t.count( {a:{$in:[2],$all:[1]}} ) );
assert.eq( 1, t.count( {a:{$in:[1],$all:[2]}} ) );

t.ensureIndex( {a:1} );
assert.eq( 1, t.count( {a:{$all:[1],$in:[2]}} ) );
assert.eq( 1, t.count( {a:{$all:[2],$in:[1]}} ) );
assert.eq( 1, t.count( {a:{$in:[2],$all:[1]}} ) );
assert.eq( 1, t.count( {a:{$in:[1],$all:[2]}} ) );
assert.eq( 1, t.count( {a:{$all:[1],$in:[2]}} ) );
