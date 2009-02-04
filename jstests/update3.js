// Update with mods corner cases.

f = db.jstests_update3;

f.drop();
f.save( { a:1 } );
f.update( {}, {$inc:{ a:1 }} );
assert.eq( 2, f.findOne().a );

f.drop();
f.save( { a:{ b: 1 } } );
f.update( {}, {$inc:{ "a.b":1 }} );
assert.eq( 2, f.findOne().a.b );

f.drop();
f.save( { a:{ b: 1 } } );
f.update( {}, {$set:{ "a.b":5 }} );
assert.eq( 5, f.findOne().a.b );

f.drop();
f.save( {'_id':0} );
f.update( {}, {$set:{'_id':5}} );
assert.eq( 0, f.findOne()._id );