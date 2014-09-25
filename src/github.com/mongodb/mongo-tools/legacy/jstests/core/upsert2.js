// A query field with a $not operator should be excluded when constructing the object to which mods
// will be applied when performing an upsert.  SERVER-8178

t = db.jstests_upsert2;

// The a:$not query operator does not cause an 'a' field to be added to the upsert document.
t.drop();
t.update( { a:{ $not:{ $lt:1 } } }, { $set:{ b:1 } }, true );
assert( !t.findOne().a );

// The a:$not query operator does not cause an 'a' field to be added to the upsert document.
t.drop();
t.update( { a:{ $not:{ $elemMatch:{ a:1 } } } }, { $set:{ b:1 } }, true );
assert( !t.findOne().a );

// The a:$not query operator does not cause an 'a' field to be added to the upsert document, and as
// a result $push can be applied to the (missing) 'a' field.
t.drop();
t.update( { a:{ $not:{ $elemMatch:{ a:1 } } } }, { $push:{ a:{ b:1, c:0 } } }, true );
assert.eq( [ { b:1, c:0 } ], t.findOne().a );
