// Test nested $or clauses SERVER-2585 SERVER-3192

t = db.jstests_orj;
t.drop();

assert.throws( function() { t.find( { x:0,$or:"a" } ).toArray(); } );
assert.throws( function() { t.find( { x:0,$or:[] } ).toArray(); } );
assert.throws( function() { t.find( { x:0,$or:[ "a" ] } ).toArray(); } );

assert.throws( function() { t.find( { x:0,$or:[{$or:"a"}] } ).toArray(); } );
assert.throws( function() { t.find( { x:0,$or:[{$or:[]}] } ).toArray(); } );
assert.throws( function() { t.find( { x:0,$or:[{$or:[ "a" ]}] } ).toArray(); } );

assert.throws( function() { t.find( { x:0,$nor:"a" } ).toArray(); } );
assert.throws( function() { t.find( { x:0,$nor:[] } ).toArray(); } );
assert.throws( function() { t.find( { x:0,$nor:[ "a" ] } ).toArray(); } );

assert.throws( function() { t.find( { x:0,$nor:[{$nor:"a"}] } ).toArray(); } );
assert.throws( function() { t.find( { x:0,$nor:[{$nor:[]}] } ).toArray(); } );
assert.throws( function() { t.find( { x:0,$nor:[{$nor:[ "a" ]}] } ).toArray(); } );

t.save( {a:1,b:2} );
assert.eq( 1, t.find( {a:1,b:2} ).itcount() );

assert.eq( 1, t.find( {a:1,$or:[{b:2}]} ).itcount() );
assert.eq( 0, t.find( {a:1,$or:[{b:3}]} ).itcount() );

assert.eq( 1, t.find( {a:1,$or:[{$or:[{b:2}]}]} ).itcount() );
assert.eq( 0, t.find( {a:1,$or:[{$or:[{b:3}]}]} ).itcount() );

assert.eq( 1, t.find( {a:1,$and:[{$or:[{$or:[{b:2}]}]}]} ).itcount() );
assert.eq( 0, t.find( {a:1,$and:[{$or:[{$or:[{b:3}]}]}]} ).itcount() );

assert.eq( 1, t.find( {$and:[{$or:[{a:1},{a:2}]},{$or:[{b:1},{b:2}]}]} ).itcount() );
assert.eq( 0, t.find( {$and:[{$or:[{a:3},{a:2}]},{$or:[{b:1},{b:2}]}]} ).itcount() );
assert.eq( 0, t.find( {$and:[{$or:[{a:1},{a:2}]},{$or:[{b:3},{b:1}]}]} ).itcount() );

assert.eq( 0, t.find( {$and:[{$nor:[{a:1},{a:2}]},{$nor:[{b:1},{b:2}]}]} ).itcount() );
assert.eq( 0, t.find( {$and:[{$nor:[{a:3},{a:2}]},{$nor:[{b:1},{b:2}]}]} ).itcount() );
assert.eq( 1, t.find( {$and:[{$nor:[{a:3},{a:2}]},{$nor:[{b:3},{b:1}]}]} ).itcount() );

assert.eq( 1, t.find( {$and:[{$or:[{a:1},{a:2}]},{$nor:[{b:1},{b:3}]}]} ).itcount() );
assert.eq( 0, t.find( {$and:[{$or:[{a:3},{a:2}]},{$nor:[{b:1},{b:3}]}]} ).itcount() );
assert.eq( 0, t.find( {$and:[{$or:[{a:1},{a:2}]},{$nor:[{b:1},{b:2}]}]} ).itcount() );
