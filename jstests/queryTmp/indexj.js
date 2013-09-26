// SERVER-726

t = db.jstests_indexj;
t.drop();

t.ensureIndex( {a:1} );
t.save( {a:5} );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( { a: { $gt:4, $lt:5 } } ).itcount());

t.drop();
t.ensureIndex( {a:1} );
t.save( {a:4} );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( { a: { $gt:4, $lt:5 } } ).itcount());

t.save( {a:5} );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( { a: { $gt:4, $lt:5 } } ).itcount());

t.save( {a:4} );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( { a: { $gt:4, $lt:5 } } ).itcount());

t.save( {a:5} );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( { a: { $gt:4, $lt:5 } } ).itcount());

t.drop();
t.ensureIndex( {a:1,b:1} );
t.save( { a:1,b:1 } );
t.save( { a:1,b:2 } );
t.save( { a:2,b:1 } );
t.save( { a:2,b:2 } );

// NEW QUERY EXPLAIN
assert.eq( 0, t.find( { a:{$in:[1,2]}, b:{$gt:1,$lt:2} } ).hint( {a:1,b:1} ).itcount() );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( { a:{$in:[1,2]}, b:{$gt:1,$lt:2} } ).hint( {a:1,b:1} ).sort( {a:-1,b:-1} ).itcount() );

t.save( {a:1,b:1} );
t.save( {a:1,b:1} );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( { a:{$in:[1,2]}, b:{$gt:1,$lt:2} } ).hint( {a:1,b:1} ).itcount() );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( { a:{$in:[1,2]}, b:{$gt:1,$lt:2} } ).hint( {a:1,b:1} ).itcount() );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( { a:{$in:[1,2]}, b:{$gt:1,$lt:2} } ).hint( {a:1,b:1} ).sort( {a:-1,b:-1} ).itcount() );

// NEW QUERY EXPLAIN
assert.eq( 0, t.find( { a:{$in:[1,1.9]}, b:{$gt:1,$lt:2} } ).hint( {a:1,b:1} ).itcount() );
// NEW QUERY EXPLAIN
assert.eq( 0, t.find( { a:{$in:[1.1,2]}, b:{$gt:1,$lt:2} } ).hint( {a:1,b:1} ).sort( {a:-1,b:-1} ).itcount() );

t.save( { a:1,b:1.5} );
// NEW QUERY EXPLAIN
assert.eq( 1, t.find( { a:{$in:[1,2]}, b:{$gt:1,$lt:2} } ).hint( {a:1,b:1} ).itcount() );
