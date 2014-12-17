// Unindexed array sorting SERVER-2884

t = db.jstests_sort9;
t.drop();

t.save( {a:[]} );
t.save( {a:[[]]} );
assert.eq( 2, t.find( {a:{$ne:4}} ).sort( {a:1} ).itcount() );
assert.eq( 2, t.find( {'a.b':{$ne:4}} ).sort( {'a.b':1} ).itcount() );
assert.eq( 2, t.find( {a:{$ne:4}} ).sort( {'a.b':1} ).itcount() );

t.drop();
t.save( {} );
assert.eq( 1, t.find( {a:{$ne:4}} ).sort( {a:1} ).itcount() );
assert.eq( 1, t.find( {'a.b':{$ne:4}} ).sort( {'a.b':1} ).itcount() );
assert.eq( 1, t.find( {a:{$ne:4}} ).sort( {'a.b':1} ).itcount() );
assert.eq( 1, t.find( {a:{$exists:0}} ).sort( {a:1} ).itcount() );
assert.eq( 1, t.find( {a:{$exists:0}} ).sort( {'a.b':1} ).itcount() );

t.drop();
t.save( {a:{}} );
assert.eq( 1, t.find( {a:{$ne:4}} ).sort( {a:1} ).itcount() );
assert.eq( 1, t.find( {'a.b':{$ne:4}} ).sort( {'a.b':1} ).itcount() );
assert.eq( 1, t.find( {a:{$ne:4}} ).sort( {'a.b':1} ).itcount() );
assert.eq( 1, t.find( {'a.b':{$exists:0}} ).sort( {a:1} ).itcount() );
assert.eq( 1, t.find( {'a.b':{$exists:0}} ).sort( {'a.b':1} ).itcount() );
