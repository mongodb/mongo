t = db.jstests_pull;
t.drop();

t.save( { a: [ 1, 2, 3 ] } );
t.update( {}, { $pull: { a: 2 } } );
t.update( {}, { $pull: { a: 6 } } );
assert.eq( [ 1, 3 ], t.findOne().a );

t.drop();
t.save( { a: [ 1, 2, 3 ] } );
t.update( {}, { $pull: { a: 2 } } );
t.update( {}, { $pull: { a: 2 } } );
assert.eq( [ 1, 3 ], t.findOne().a );

t.drop();
t.save( { a: [ 2 ] } );
t.update( {}, { $pull: { a: 2 } } );
t.update( {}, { $pull: { a: 6 } } );
assert.eq( [], t.findOne().a );
