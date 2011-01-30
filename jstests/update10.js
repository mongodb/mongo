f = db.ed_db_update10;

f.drop();
f.save( { a: 4 } );
f.update( { a: 4 }, { $min: { a: 2 } } );
assert.eq( 2, f.findOne().a );

f.drop();
f.save( { a: 4 } );
f.update( { a: 4 }, { $min: { a: 6 } } );
assert.eq( 4, f.findOne().a );

f.drop();
f.save( { a: 4 } );
f.update( { a: 4 }, { $max: { a: 2 } } );
assert.eq( 4, f.findOne().a );

f.drop();
f.save( { a: 4 } );
f.update( { a: 4 }, { $max: { a: 6 } } );
assert.eq( 6, f.findOne().a );

