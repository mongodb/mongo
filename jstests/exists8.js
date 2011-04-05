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
