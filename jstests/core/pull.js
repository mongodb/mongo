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

// SERVER-6047: $pull creates empty nested docs for dotted fields
// that don't exist.
t.drop()
t.save({ m : 1 } );
t.update( { m : 1 }, { $pull : { 'a.b' : [ 1 ] } } );
assert( ('a' in t.findOne()) == false );
// Non-obvious bit: the implementation of non-in-place update
// might do different things depending on whether the "new" field
// comes before or after existing fields in the document.
// So for now it's worth testing that too. Sorry, future; blame the past.
t.update( { m : 1 }, { $pull : { 'x.y' : [ 1 ] } } );
assert( ('z' in t.findOne()) == false );
// End SERVER-6047  
