// Sparse indexes with arrays SERVER-3216

t = db.jstests_indext;
t.drop();

t.ensureIndex( {'a.b':1}, {sparse:true} );
t.save( {a:[]} );
t.save( {a:1} );
assert.eq( 0, t.find().hint( {'a.b':1} ).itcount() );
assert.eq( 0, t.find().hint( {'a.b':1} ).explain().nscanned );

t.ensureIndex( {'a.b':1,'a.c':1}, {sparse:true} );
t.save( {a:[]} );
t.save( {a:1} );
assert.eq( 0, t.find().hint( {'a.b':1,'a.c':1} ).itcount() );
assert.eq( 0, t.find().hint( {'a.b':1,'a.c':1} ).explain().nscanned );

t.save( {a:[{b:1}]} );
t.save( {a:1} );
assert.eq( 1, t.find().hint( {'a.b':1,'a.c':1} ).itcount() );
assert.eq( 1, t.find().hint( {'a.b':1,'a.c':1} ).explain().nscanned );
