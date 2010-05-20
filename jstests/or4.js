t = db.jstests_or4;
t.drop();

t.ensureIndex( {a:1} );
t.ensureIndex( {b:1} );

t.save( {a:2} );
t.save( {b:3} );
t.save( {b:3} );
t.save( {a:2,b:3} );

assert.eq.automsg( "4", "t.count( {$or:[{a:2},{b:3}]} )" );
assert.eq.automsg( "2", "t.count( {$or:[{a:2},{a:2}]} )" );

assert.eq.automsg( "2", "t.find( {} ).skip( 2 ).count( true )" );
assert.eq.automsg( "2", "t.find( {$or:[{a:2},{b:3}]} ).skip( 2 ).count( true )" );
assert.eq.automsg( "1", "t.find( {$or:[{a:2},{b:3}]} ).skip( 3 ).count( true )" );

assert.eq.automsg( "2", "t.find( {} ).limit( 2 ).count( true )" );
assert.eq.automsg( "1", "t.find( {$or:[{a:2},{b:3}]} ).limit( 1 ).count( true )" );
assert.eq.automsg( "2", "t.find( {$or:[{a:2},{b:3}]} ).limit( 2 ).count( true )" );
assert.eq.automsg( "3", "t.find( {$or:[{a:2},{b:3}]} ).limit( 3 ).count( true )" );
assert.eq.automsg( "4", "t.find( {$or:[{a:2},{b:3}]} ).limit( 4 ).count( true )" );

t.remove({ $or: [{ a: 2 }, { b: 3}] });
assert.eq.automsg( "0", "t.count()" );

t.save( {b:3} );
t.remove({ $or: [{ a: 2 }, { b: 3}] });
assert.eq.automsg( "0", "t.count()" );

t.save( {a:2} );
t.save( {b:3} );
t.save( {a:2,b:3} );

t.update( {$or:[{a:2},{b:3}]}, {$set:{z:1}}, false, true );
assert.eq.automsg( "3", "t.count( {z:1} )" );

assert.eq.automsg( "3", "t.find( {$or:[{a:2},{b:3}]} ).toArray().length" );

