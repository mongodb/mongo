// Check sorting of array sub field SERVER-480.

t = db.jstests_sort7;
t.drop();

// Compare indexed and unindexed sort order for an array embedded field.

t.save( { a : [ { x : 2 } ] } );
t.save( { a : [ { x : 1 } ] } );
t.save( { a : [ { x : 3 } ] } );
unindexed = t.find().sort( {"a.x":1} ).toArray();
t.ensureIndex( { "a.x" : 1 } );
indexed = t.find().sort( {"a.x":1} ).hint( {"a.x":1} ).toArray();
assert.eq( unindexed, indexed );

// Now check when there are two objects in the array.

t.remove();
t.save( { a : [ { x : 2 }, { x : 3 } ] } );
t.save( { a : [ { x : 1 }, { x : 4 } ] } );
t.save( { a : [ { x : 3 }, { x : 2 } ] } );
unindexed = t.find().sort( {"a.x":1} ).toArray();
t.ensureIndex( { "a.x" : 1 } );
indexed = t.find().sort( {"a.x":1} ).hint( {"a.x":1} ).toArray();
assert.eq( unindexed, indexed );
