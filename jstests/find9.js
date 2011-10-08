t = db.jstests_find9;
t.drop();

id1 = new ObjectId();
id2 = new ObjectId();

t.save( {a:id1} );
assert.eq( 1, t.count( { a: id1.valueOf() } ) );
assert.eq( 1, t.count( { a: {$ne: id2.valueOf() }} ) );
assert.eq( 0, t.count( { a: id2.valueOf() } ) );


assert.eq( 1,  t.find( { a: id1.valueOf() } ).toArray().length );
assert.eq( 0, t.find( { a: id2.valueOf() } ).toArray().length );


assert( t.findOne( { a: id1.valueOf() } ) );
assert( ! t.findOne( { a: id2.valueOf() } ) );


assert.eq( 0, t.find( { a: id1.valueOf() }, null, null, null, null, null, true ).toArray().length );
assert.eq( null, t.findOne( { a: id1.valueOf() }, null, null, true ) );
