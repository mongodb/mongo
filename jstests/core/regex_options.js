t = db.jstests_regex_options;

t.drop();
t.save( { a: "foo" } );
assert.eq( 1, t.count( { a: { "$regex": /O/i } } ) );
assert.eq( 1, t.count( { a: /O/i } ) );
assert.eq( 1, t.count( { a: { "$regex": "O", "$options": "i" } } ) );
