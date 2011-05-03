// Test $exists with array element field names SERVER-2897

t = db.jstests_exists8;
t.drop();

t.save( {a:[1]} );
assert.eq( 1, t.count( {'a.0':{$exists:true}} ) );
assert.eq( 1, t.count( {'a.1':{$exists:false}} ) );
assert.eq( 0, t.count( {'a.0':{$exists:false}} ) );
assert.eq( 0, t.count( {'a.1':{$exists:true}} ) );

t.remove();
t.save( {a:[1,2]} );
assert.eq( 1, t.count( {'a.0':{$exists:true}} ) );
assert.eq( 0, t.count( {'a.1':{$exists:false}} ) );
assert.eq( 0, t.count( {'a.0':{$exists:false}} ) );
assert.eq( 1, t.count( {'a.1':{$exists:true}} ) );

t.remove();
t.save( {a:[{}]} );
assert.eq( 1, t.count( {'a.0':{$exists:true}} ) );
assert.eq( 1, t.count( {'a.1':{$exists:false}} ) );
assert.eq( 0, t.count( {'a.0':{$exists:false}} ) );
assert.eq( 0, t.count( {'a.1':{$exists:true}} ) );

t.remove();
t.save( {a:[{},{}]} );
assert.eq( 1, t.count( {'a.0':{$exists:true}} ) );
assert.eq( 0, t.count( {'a.1':{$exists:false}} ) );
assert.eq( 0, t.count( {'a.0':{$exists:false}} ) );
assert.eq( 1, t.count( {'a.1':{$exists:true}} ) );

t.remove();
t.save( {a:[{'b':2},{'a':1}]} );
assert.eq( 1, t.count( {'a.a':{$exists:true}} ) );
assert.eq( 1, t.count( {'a.1.a':{$exists:true}} ) );
assert.eq( 1, t.count( {'a.0.a':{$exists:false}} ) );

t.remove();
t.save( {a:[[1]]} );
assert.eq( 1, t.count( {'a.0':{$exists:true}} ) );
assert.eq( 1, t.count( {'a.0.0':{$exists:true}} ) );
assert.eq( 0, t.count( {'a.0.0':{$exists:false}} ) );
assert.eq( 0, t.count( {'a.0.0.0':{$exists:true}} ) );
assert.eq( 1, t.count( {'a.0.0.0':{$exists:false}} ) );

t.remove();
t.save( {a:[[[1]]]} );
assert.eq( 1, t.count( {'a.0.0.0':{$exists:true}} ) );

t.remove();
t.save( {a:[[{b:1}]]} );
assert.eq( 0, t.count( {'a.b':{$exists:true}} ) );
assert.eq( 1, t.count( {'a.b':{$exists:false}} ) );
assert.eq( 1, t.count( {'a.0.b':{$exists:true}} ) );
assert.eq( 0, t.count( {'a.0.b':{$exists:false}} ) );

t.remove();
t.save( {a:[[],[{b:1}]]} );
assert.eq( 0, t.count( {'a.0.b':{$exists:true}} ) );
assert.eq( 1, t.count( {'a.0.b':{$exists:false}} ) );

t.remove();
t.save( {a:[[],[{b:1}]]} );
assert.eq( 1, t.count( {'a.1.b':{$exists:true}} ) );
assert.eq( 0, t.count( {'a.1.b':{$exists:false}} ) );

t.remove();
t.save( {a:[[],[{b:1}]]} );
assert.eq( 1, t.count( {'a.1.0.b':{$exists:true}} ) );
assert.eq( 0, t.count( {'a.1.0.b':{$exists:false}} ) );

t.remove();
t.save( {a:[[],[{b:1}]]} );
assert.eq( 0, t.count( {'a.1.1.b':{$exists:true}} ) );
assert.eq( 1, t.count( {'a.1.1.b':{$exists:false}} ) );
