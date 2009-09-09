t = db.jstests_exists;
t.drop();

t.save( {} );
t.save( {a:1} );
t.save( {a:{b:1}} );
t.save( {a:{b:{c:1}}} );
t.save( {a:{b:{c:{d:null}}}} );

assert.eq( 5, t.count() );
assert.eq( 1, t.count( {a:null} ) );
assert.eq( 2, t.count( {'a.b':null} ) );
assert.eq( 3, t.count( {'a.b.c':null} ) );
assert.eq( 5, t.count( {'a.b.c.d':null} ) );

assert.eq( 5, t.count() );
assert.eq( 4, t.count( {a:{$ne:null}} ) );
assert.eq( 3, t.count( {'a.b':{$ne:null}} ) );
assert.eq( 2, t.count( {'a.b.c':{$ne:null}} ) );
assert.eq( 0, t.count( {'a.b.c.d':{$ne:null}} ) );

assert.eq( 4, t.count( {a: {$exists:true}} ) );
assert.eq( 3, t.count( {'a.b': {$exists:true}} ) );
assert.eq( 2, t.count( {'a.b.c': {$exists:true}} ) );
assert.eq( 1, t.count( {'a.b.c.d': {$exists:true}} ) );

assert.eq( 1, t.count( {a: {$exists:false}} ) );
assert.eq( 2, t.count( {'a.b': {$exists:false}} ) );
assert.eq( 3, t.count( {'a.b.c': {$exists:false}} ) );
assert.eq( 4, t.count( {'a.b.c.d': {$exists:false}} ) );

t.drop();

t.save( {r:[{s:1}]} );
assert( t.findOne( {'r.s':{$exists:true}} ) );
assert( !t.findOne( {'r.s':{$exists:false}} ) );
assert( !t.findOne( {'r.t':{$exists:true}} ) );
assert( t.findOne( {'r.t':{$exists:false}} ) );
