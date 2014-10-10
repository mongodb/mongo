// Test some $not/$exists cases.

t = db.jstests_exists5;
t.drop();

t.save( {a:1} );
assert.eq( 1, t.count( {'a.b':{$exists:false}} ) );
assert.eq( 1, t.count( {'a.b':{$not:{$exists:true}}} ) );
assert.eq( 1, t.count( {'c.d':{$not:{$exists:true}}} ) );
assert.eq( 0, t.count( {'a.b':{$exists:true}} ) );
assert.eq( 0, t.count( {'a.b':{$not:{$exists:false}}} ) );
assert.eq( 0, t.count( {'c.d':{$not:{$exists:false}}} ) );

t.drop();
t.save( {a:{b:1}} );
assert.eq( 1, t.count( {'a.b':{$exists:true}} ) );
assert.eq( 1, t.count( {'a.b':{$not:{$exists:false}}} ) );
assert.eq( 0, t.count( {'a.b':{$exists:false}} ) );
assert.eq( 0, t.count( {'a.b':{$not:{$exists:true}}} ) );

t.drop();
t.save( {a:[1]} );
assert.eq( 1, t.count( {'a.b':{$exists:false}} ) );
assert.eq( 1, t.count( {'a.b':{$not:{$exists:true}}} ) );
assert.eq( 0, t.count( {'a.b':{$exists:true}} ) );
assert.eq( 0, t.count( {'a.b':{$not:{$exists:false}}} ) );

t.drop();
t.save( {a:[{b:1}]} );
assert.eq( 1, t.count( {'a.b':{$exists:true}} ) );
assert.eq( 1, t.count( {'a.b':{$not:{$exists:false}}} ) );
assert.eq( 0, t.count( {'a.b':{$exists:false}} ) );
assert.eq( 0, t.count( {'a.b':{$not:{$exists:true}}} ) );
