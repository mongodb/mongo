// SERVER-1735 $type:10 matches null value, not missing value.

t = db.jstests_type2;
t.drop();

t.save( {a:null} );
t.save( {} );
t.save( {a:'a'} );

function test() {
	assert.eq( 2, t.count( {a:null} ) );
	assert.eq( 1, t.count( {a:{$type:10}} ) );
	assert.eq( 2, t.count( {a:{$exists:true}} ) );
	assert.eq( 1, t.count( {a:{$exists:false}} ) );
}

test();
t.ensureIndex( {a:1} );
test();