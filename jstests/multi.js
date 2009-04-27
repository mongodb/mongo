t = db.jstests_multi;
t.drop();

t.ensureIndex( { a: 1 } );
t.save( { a: [ 1, 2 ] } );
assert.eq( 1, t.find( { a: { $gt: 0 } } ).count() );
assert.eq( 1, t.find( { a: { $gt: 0 } } ).toArray().length );

t.drop();
t.save( { a: [ [ [ 1 ] ] ] } );
assert.eq( 0, t.find( { a:1 } ).count() );
assert.eq( 0, t.find( { a: [ 1 ] } ).count() );
assert.eq( 1, t.find( { a: [ [ 1 ] ] } ).count() );
assert.eq( 0, t.find( { a: [ [ [ 1 ] ] ] } ).count() );

t.drop();
t.save( { a: [ 1, 2 ] } );
assert.eq( 0, t.find( { a: { $ne: 1 } } ).count() );

t.drop();
t.save( { a: [ { b: 1 }, { b: 2 } ] } );
assert.eq( 0, t.find( { 'a.b': { $ne: 1 } } ).count() );
