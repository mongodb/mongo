t = db.jstests_regex;

t.drop();
t.save( { a: "bcd" } );
assert.eq( 1, t.count( { a: /b/ } ) , "A" );
assert.eq( 1, t.count( { a: /bc/ } ) , "B" );
assert.eq( 1, t.count( { a: /bcd/ } ) , "C" );
assert.eq( 0, t.count( { a: /bcde/ } ) , "D" );

t.drop();
t.save( { a: { b: "cde" } } );
assert.eq( 1, t.count( { 'a.b': /de/ } ) , "E" );

t.drop();
t.save( { a: { b: [ "cde" ] } } );
assert.eq( 1, t.count( { 'a.b': /de/ } ) , "F" );

t.drop();
t.save( { a: [ { b: "cde" } ] } );
assert.eq( 1, t.count( { 'a.b': /de/ } ) , "G" );

t.drop();
t.save( { a: [ { b: [ "cde" ] } ] } );
assert.eq( 1, t.count( { 'a.b': /de/ } ) , "H" );
