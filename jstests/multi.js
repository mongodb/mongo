t = db.jstests_multi;
t.drop();

t.ensureIndex( { a: 1 } );
t.save( { a: [ 1, 2 ] } );
assert.eq( 1, t.find( { a: { $gt: 0 } } ).count() );
