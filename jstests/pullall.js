t = db.jstests_pullall;
t.drop();

t.save( { a: [ 1, 2, 3 ] } );
t.update( {}, { $pullAll: { a: [ 3 ] } } );
assert.eq( [ 1, 2 ], t.findOne().a );
t.update( {}, { $pullAll: { a: [ 3 ] } } );
assert.eq( [ 1, 2 ], t.findOne().a );

t.drop();
t.save( { a: [ 1, 2, 3 ] } );
t.update( {}, { $pullAll: { a: [ 2, 3 ] } } );
assert.eq( [ 1 ], t.findOne().a );
t.update( {}, { $pullAll: { a: [] } } );
assert.eq( [ 1 ], t.findOne().a );
t.update( {}, { $pullAll: { a: [ 1, 5 ] } } );
assert.eq( [], t.findOne().a );

