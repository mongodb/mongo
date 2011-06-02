// SERVER-1089 Test and/or nesting

t = db.jstests_andor;
t.drop();

// not ok
function ok( q ) {
	assert.eq( 1, t.find( q ).itcount() );
}

// throws
function throws( q ) {
    // count() will currently just return 0 rather than assert.
	assert.throws( function() { t.find( q ).itcount(); } );
}

t.save( {a:1} );

function test() {
    
    ok( {a:1} );
    
    ok( {$and:[{a:1}]} );
    ok( {$or:[{a:1}]} );
    
    ok( {$and:[{$and:[{a:1}]}]} );
    throws( {$or:[{$or:[{a:1}]}]} );
    
    throws( {$and:[{$or:[{a:1}]}]} );
    ok( {$or:[{$and:[{a:1}]}]} );
    
    throws( {$and:[{$and:[{$or:[{a:1}]}]}]} );
    throws( {$and:[{$or:[{$and:[{a:1}]}]}]} );
    ok( {$or:[{$and:[{$and:[{a:1}]}]}]} );
    
    throws( {$or:[{$and:[{$or:[{a:1}]}]}]} );
    
    // now test $nor
    
    ok( {$and:[{a:1}]} );
    ok( {$nor:[{a:2}]} );
    
    ok( {$and:[{$and:[{a:1}]}]} );
    throws( {$nor:[{$nor:[{a:1}]}]} );
    
    throws( {$and:[{$nor:[{a:2}]}]} );
    ok( {$nor:[{$and:[{a:2}]}]} );
    
    throws( {$and:[{$and:[{$nor:[{a:2}]}]}]} );
    throws( {$and:[{$nor:[{$and:[{a:2}]}]}]} );
    ok( {$nor:[{$and:[{$and:[{a:2}]}]}]} );
    
    throws( {$nor:[{$and:[{$nor:[{a:1}]}]}]} );
    
}

test();
t.ensureIndex( {a:1} );
test();