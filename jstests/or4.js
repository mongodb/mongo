t = db.jstests_or4;
t.drop();

t.ensureIndex( {a:1} );
t.ensureIndex( {b:1} );

t.save( {a:2} );
t.save( {b:3} );
t.save( {a:2,b:3} );

assert.eq( 3, t.count( {$or:[{a:2},{b:3}]} ), 'a' );
assert.eq( 2, t.count( {$or:[{a:2},{a:2}]} ), 'b' );

t.remove({ $or: [{ a: 2 }, { b: 3}] });

print("count:" + t.count());

assert.eq( 0, t.count(), 'count' );

t.save( {a:2} );
t.save( {b:3} );
t.save( {a:2,b:3} );

t.update( {$or:[{a:2},{b:3}]}, {$set:{z:1}}, false, true );
assert.eq( 3, t.count( {z:1} ) );

assert.eq( 3, t.find( {$or:[{a:2},{b:3}]} ).toArray().length );



