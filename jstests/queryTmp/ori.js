// Check elimination of proper range type when popping a $or clause SERVER-958.

t = db.jstests_ori;
t.drop();

t.ensureIndex( {a:1,b:1} );
t.ensureIndex( {a:1,c:1} );

t.save( {a:1,b:[2,3],c:4} );
t.save( {a:10,b:2,c:4} );

// Check that proper results are returned.

assert.eq( 2, t.count( {$or:[{a:{$gt:0,$lt:5},b:2},{a:10,c:4}]} ) );
// Two $or clauses expected to be scanned.
assert.eq( 2, t.find( {$or:[{a:{$gt:0,$lt:5},b:2},{a:10,c:4}]} ).explain().clauses.length );
assert.eq( 2, t.count( {$or:[{a:10,b:2},{a:{$gt:0,$lt:5},c:4}]} ) );

t.drop();

// Now try a different index order.

t.ensureIndex( {b:1,a:1} );
t.ensureIndex( {a:1,c:1} );

t.save( {a:1,b:[2,3],c:4} );
t.save( {a:10,b:2,c:4} );

assert.eq( 2, t.count( {$or:[{a:{$gt:0,$lt:5},b:2},{a:10,c:4}]} ) );
assert.eq( 2, t.count( {$or:[{a:10,b:2},{a:{$gt:0,$lt:5},c:4}]} ) );

t.drop();

// Now eliminate a range.

t.ensureIndex( {a:1} );
t.ensureIndex( {b:1} );

t.save( {a:[1,2],b:1} );
t.save( {a:10,b:1} );

assert.eq( 2, t.count( {$or:[{a:{$gt:0,$lt:5}},{a:10,b:1}]} ) );
// Because a:1 is multikey, the value a:10 is scanned with the first clause.
assert.isnull( t.find( {$or:[{a:{$gt:0,$lt:5}},{a:10,b:1}]} ).explain().clauses );

assert.eq( 2, t.count( {$or:[{a:{$lt:5,$gt:0}},{a:10,b:1}]} ) );
// Now a:10 is not scanned in the first clause so the second clause is not eliminated.
assert.eq( 2, t.find( {$or:[{a:{$lt:5,$gt:0}},{a:10,b:1}]} ).explain().clauses.length );
