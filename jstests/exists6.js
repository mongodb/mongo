// SERVER-393 Test indexed matching with $exists.

t = db.jstests_exists6;
t.drop();

t.ensureIndex( {a:1,b:1} );
t.save( {a:1} );

assert.eq( 1, t.find( {a:1,b:{$exists:false}} ).itcount() );
