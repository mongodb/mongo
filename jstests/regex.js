t = db.jstests_regex;

t.drop();
t.save( { a: "bcd" } );
assert.eq( 1, t.count( { a: /b/ } ) );
assert.eq( 1, t.count( { a: /bc/ } ) );
assert.eq( 1, t.count( { a: /bcd/ } ) );
assert.eq( 0, t.count( { a: /bcde/ } ) );

t.drop();
t.save( { a: { b: "cde" } } );
assert.eq( 1, t.count( { 'a.b': /de/ } ) );

t.drop();
t.save( { a: { b: [ "cde" ] } } );
assert.eq( 1, t.count( { 'a.b': /de/ } ) );

t.drop();
t.save( { a: [ { b: "cde" } ] } );
assert.eq( 1, t.count( { 'a.b': /de/ } ) );

t.drop();
t.save( { a: [ { b: [ "cde" ] } ] } );
assert.eq( 1, t.count( { 'a.b': /de/ } ) );
