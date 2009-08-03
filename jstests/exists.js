t = db.jstests_exists;
t.drop();

t.save( {} );
t.save( {a:1} );
t.save( {a:{b:1}} );
t.save( {a:{b:{c:1}}} );

assert.eq( 4, t.count() );
assert.eq( 3, t.count( {a:{$ne:null}} ) );
assert.eq( 2, t.count( {'a.b':{$ne:null}} ) );
assert.eq( 1, t.count( {'a.b.c':{$ne:null}} ) );
