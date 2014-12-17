// Test fast mode count with multikey entries.

t = db.jstests_count9;
t.drop();

t.ensureIndex( {a:1} );

t.save( {a:['a','b','a']} );
assert.eq( 1, t.count( {a:'a'} ) );

t.save( {a:['a','b','a']} );
assert.eq( 2, t.count( {a:'a'} ) );

t.drop();
t.ensureIndex( {a:1,b:1} );

t.save( {a:['a','b','a'],b:'r'} );
assert.eq( 1, t.count( {a:'a',b:'r'} ) );
assert.eq( 1, t.count( {a:'a'} ) );

t.save( {a:['a','b','a'],b:'r'} );
assert.eq( 2, t.count( {a:'a',b:'r'} ) );
assert.eq( 2, t.count( {a:'a'} ) );

t.drop();
t.ensureIndex( {'a.b':1,'a.c':1} );
t.save( {a:[{b:'b',c:'c'},{b:'b',c:'c'}]} );
assert.eq( 1, t.count( {'a.b':'b','a.c':'c'} ) );
