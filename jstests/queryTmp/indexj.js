// SERVER-726

t = db.jstests_indexj;
t.drop();

t.ensureIndex( {a:1} );
t.save( {a:5} );
assert.eq( 0, t.find( { a: { $gt:4, $lt:5 } } ).explain().nscanned, "A" );

t.drop();
t.ensureIndex( {a:1} );
t.save( {a:4} );
assert.eq( 0, t.find( { a: { $gt:4, $lt:5 } } ).explain().nscanned, "B" );

t.save( {a:5} );
assert.eq( 0, t.find( { a: { $gt:4, $lt:5 } } ).explain().nscanned, "D" );

t.save( {a:4} );
assert.eq( 0, t.find( { a: { $gt:4, $lt:5 } } ).explain().nscanned, "C" );

t.save( {a:5} );
assert.eq( 0, t.find( { a: { $gt:4, $lt:5 } } ).explain().nscanned, "D" );

t.drop();
t.ensureIndex( {a:1,b:1} );
t.save( { a:1,b:1 } );
t.save( { a:1,b:2 } );
t.save( { a:2,b:1 } );
t.save( { a:2,b:2 } );

assert.eq( 2, t.find( { a:{$in:[1,2]}, b:{$gt:1,$lt:2} } ).hint( {a:1,b:1} ).explain().nscanned );
assert.eq( 2, t.find( { a:{$in:[1,2]}, b:{$gt:1,$lt:2} } ).hint( {a:1,b:1} ).sort( {a:-1,b:-1} ).explain().nscanned );

t.save( {a:1,b:1} );
t.save( {a:1,b:1} );
assert.eq( 2, t.find( { a:{$in:[1,2]}, b:{$gt:1,$lt:2} } ).hint( {a:1,b:1} ).explain().nscanned );
assert.eq( 2, t.find( { a:{$in:[1,2]}, b:{$gt:1,$lt:2} } ).hint( {a:1,b:1} ).explain().nscanned );
assert.eq( 2, t.find( { a:{$in:[1,2]}, b:{$gt:1,$lt:2} } ).hint( {a:1,b:1} ).sort( {a:-1,b:-1} ).explain().nscanned );

assert.eq( 1, t.find( { a:{$in:[1,1.9]}, b:{$gt:1,$lt:2} } ).hint( {a:1,b:1} ).explain().nscanned );
assert.eq( 1, t.find( { a:{$in:[1.1,2]}, b:{$gt:1,$lt:2} } ).hint( {a:1,b:1} ).sort( {a:-1,b:-1} ).explain().nscanned );

t.save( { a:1,b:1.5} );
assert.eq( 3, t.find( { a:{$in:[1,2]}, b:{$gt:1,$lt:2} } ).hint( {a:1,b:1} ).explain().nscanned, "F" );
