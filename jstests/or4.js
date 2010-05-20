t = db.jstests_or4;
t.drop();

t.ensureIndex( {a:1} );
t.ensureIndex( {b:1} );

t.save( {a:2} );
t.save( {b:3} );
t.save( {a:2,b:3} );

assert.eq.automsg( "3", "t.count( {$or:[{a:2},{b:3}]} )" );
assert.eq.automsg( "2", "t.count( {$or:[{a:2},{a:2}]} )" );
t.remove({ $or: [{ a: 2 }, { b: 3}] });

assert.eq.automsg( "0", "t.count()" );

t.save( {a:2} );
t.save( {b:3} );
t.save( {a:2,b:3} );

t.update( {$or:[{a:2},{b:3}]}, {$set:{z:1}}, false, true );
assert.eq.automsg( "3", "t.count( {z:1} )" );

assert.eq.automsg( "3", "t.find( {$or:[{a:2},{b:3}]} ).toArray().length" );

