t = db.jstests_or5;
t.drop();

t.ensureIndex( {a:1} );
t.ensureIndex( {b:1} );

assert.eq.automsg( "'BasicCursor'", "t.find( {$or:[{a:2},{b:3},{}]} ).explain().cursor" );
assert.eq.automsg( "'BasicCursor'", "t.find( {$or:[{a:2},{b:3},{c:4}]} ).explain().cursor" );
printjson( t.find( {$or:[{a:2},{b:3}]} ).sort( {c:1} ).explain() );
assert.eq.automsg( "'BasicCursor'", "t.find( {$or:[{a:2},{b:3}]} ).sort( {c:1} ).explain().cursor" );
e = t.find( {$or:[{a:2},{b:3}]} ).sort( {a:1} ).explain();
assert.eq.automsg( "'BtreeCursor a_1'", "e.cursor" );
assert.eq.automsg( "1", "e.indexBounds.a[ 0 ][ 0 ].$minElement" );
assert.eq.automsg( "1", "e.indexBounds.a[ 0 ][ 1 ].$maxElement" );

t.ensureIndex( {c:1} );

t.save( {a:2} );
t.save( {b:3} );
t.save( {c:4} );
t.save( {a:2,b:3} );
t.save( {a:2,c:4} );
t.save( {b:3,c:4} );
t.save( {a:2,b:3,c:4} );

assert.eq.automsg( "7", "t.count( {$or:[{a:2},{b:3},{c:4}]} )" );
assert.eq.automsg( "6", "t.count( {$or:[{a:6},{b:3},{c:4}]} )" );
assert.eq.automsg( "6", "t.count( {$or:[{a:2},{b:6},{c:4}]} )" );
assert.eq.automsg( "6", "t.count( {$or:[{a:2},{b:3},{c:6}]} )" );

assert.eq.automsg( "7", "t.find( {$or:[{a:2},{b:3},{c:4}]} ).toArray().length" );
assert.eq.automsg( "6", "t.find( {$or:[{a:6},{b:3},{c:4}]} ).toArray().length" );
assert.eq.automsg( "6", "t.find( {$or:[{a:2},{b:6},{c:4}]} ).toArray().length" );
assert.eq.automsg( "6", "t.find( {$or:[{a:2},{b:3},{c:6}]} ).toArray().length" );

for( i = 2; i <= 7; ++i ) {
assert.eq.automsg( "7", "t.find( {$or:[{a:2},{b:3},{c:4}]} ).batchSize( i ).toArray().length" );
assert.eq.automsg( "6", "t.find( {$or:[{a:6},{b:3},{c:4}]} ).batchSize( i ).toArray().length" );
assert.eq.automsg( "6", "t.find( {$or:[{a:2},{b:6},{c:4}]} ).batchSize( i ).toArray().length" );
assert.eq.automsg( "6", "t.find( {$or:[{a:2},{b:3},{c:6}]} ).batchSize( i ).toArray().length" );
}

t.ensureIndex( {z:"2d"} );

assert.eq.automsg( "'GeoSearchCursor'", "t.find( {z:{$near:[50,50]},a:2} ).explain().cursor" );
assert.eq.automsg( "'GeoSearchCursor'", "t.find( {z:{$near:[50,50]},$or:[{a:2}]} ).explain().cursor" );
assert.eq.automsg( "'GeoSearchCursor'", "t.find( {$or:[{a:2}],z:{$near:[50,50]}} ).explain().cursor" );
assert.eq.automsg( "'GeoSearchCursor'", "t.find( {$or:[{a:2},{b:3}],z:{$near:[50,50]}} ).explain().cursor" );
assert.throws.automsg( function() { return t.find( {$or:[{z:{$near:[50,50]}},{a:2}]} ).toArray(); } );

function reset() {
    t.drop();
    
    t.ensureIndex( {a:1} );
    t.ensureIndex( {b:1} );
    t.ensureIndex( {c:1} );
    
    t.save( {a:2} );
    t.save( {a:2} );
    t.save( {b:3} );
    t.save( {b:3} );
    t.save( {c:4} );
    t.save( {c:4} );
}

reset();

assert.eq.automsg( "6", "t.find( {$or:[{a:2},{b:3},{c:4}]} ).batchSize( 1 ).itcount()" );
assert.eq.automsg( "6", "t.find( {$or:[{a:2},{b:3},{c:4}]} ).batchSize( 2 ).itcount()" );

c = t.find( {$or:[{a:2},{b:3},{c:4}]} ).batchSize( 2 );
c.next(); // Trigger initial query.
t.remove( {b:3} );
db.getLastError();
assert.eq.automsg( "3", c.itcount() ); // The remaining [{a:2},{c:4},{c:4}] comprise 3 results.

reset();

c = t.find( {$or:[{a:2},{b:3},{c:4}]} ).batchSize( 2 );
c.next();
c.next();
t.remove( {b:3} );
db.getLastError();
assert.eq.automsg( "2", c.itcount() );

reset();

c = t.find( {$or:[{a:2},{b:3},{c:4}]} ).batchSize( 2 );
c.next();
c.next();
c.next();
t.remove( {b:3} );
db.getLastError();
assert.eq.automsg( "3", c.itcount() );

reset();

c = t.find( {$or:[{a:2},{b:3},{c:4}]} ).batchSize( 2 );
c.next();
c.next();
c.next();
c.next();
t.remove( {b:3} );
db.getLastError();
assert.eq.automsg( "2", c.itcount() );

t.drop();

t.save( {a:[1,2]} );
assert.eq.automsg( "1", "t.find( {$or:[{a:[1,2]}]} ).itcount()" );
assert.eq.automsg( "1", "t.find( {$or:[{a:{$all:[1,2]}}]} ).itcount()" );
assert.eq.automsg( "0", "t.find( {$or:[{a:{$all:[1,3]}}]} ).itcount()" );
