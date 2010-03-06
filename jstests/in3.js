t = db.jstests_in3;

t.drop();
t.ensureIndex( {i:1} );
assert.eq( [ [ {i:3}, {i:3} ] ], t.find( {i:{$in:[3]}} ).explain().indexBounds );
assert.eq( [ [ {i:3}, {i:3} ], [ {i:6}, {i:6} ] ], t.find( {i:{$in:[3,6]}} ).explain().indexBounds );
