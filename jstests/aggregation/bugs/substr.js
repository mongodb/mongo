// Aggregation $substr tests.

t = db.jstests_aggregation_substr;
t.drop();

t.save( {} );

function assertSubstring( expected, str, offset, len ) {
    assert.eq( expected,
               t.aggregate( { $project:{ a:{ $substr:[ str, offset, len ] } } } ).result[ 0 ].a );
}

function assertArgsException( args ) {
    assert.commandFailed(t.runCommand('aggregate', {pipeline: [{$substr: args}]}));
}

function assertException( str, offset, len ) {
    assertArgsException([str, offset, len]);
}

// Wrong number of arguments.
assertArgsException( [] );
assertArgsException( [ 'foo' ] );
assertArgsException( [ 'foo', 1 ] );
assertArgsException( [ 'foo', 1, 1, 1 ] );

// Basic offset / length checks.
assertSubstring( 'abcd', 'abcd', 0, 4 );
assertSubstring( 'abcd', 'abcd', 0, 5 );
assertSubstring( '', 'abcd', -1 /* unsigned */, 4 );
assertSubstring( 'a', 'abcd', 0, 1 );
assertSubstring( 'ab', 'abcd', 0, 2 );
assertSubstring( 'b', 'abcd', 1, 1 );
assertSubstring( 'd', 'abcd', 3, 1 );
assertSubstring( '', 'abcd', 4, 1 );
assertSubstring( '', 'abcd', 3, 0 );
assertSubstring( 'cd', 'abcd', 2, -1 /* unsigned */ );

// See server6186.js for additional offset / length checks.

// Additional numeric types for offset / length.
assertSubstring( 'bc', 'abcd', 1, 2 );
assertSubstring( 'bc', 'abcd', 1.0, 2.0 );
assertSubstring( 'bc', 'abcd', NumberInt( 1 ), NumberInt( 2 ) );
assertSubstring( 'bc', 'abcd', NumberLong( 1 ), NumberLong( 2 ) );
assertSubstring( 'bc', 'abcd', NumberInt( 1 ), NumberLong( 2 ) );
assertSubstring( 'bc', 'abcd', NumberLong( 1 ), NumberInt( 2 ) );
// Integer component is used.
assertSubstring( 'bc', 'abcd', 1.2, 2.2 );
assertSubstring( 'bc', 'abcd', 1.9, 2.9 );

// Non numeric types for offset / length.
assertException( 'abcd', false, 2 );
assertException( 'abcd', 1, true );
assertException( 'abcd', 'q', 2 );
assertException( 'abcd', 1, 'r' );
assertException( 'abcd', null, 3 );
assertException( 'abcd', 1, undefined );

// String coercion.
assertSubstring( '123', 123, 0, 3 );
assertSubstring( '2', 123, 1, 1 );
assertSubstring( '1970', new Date( 0 ), 0, 4 );
assertSubstring( '', null, 0, 4 );
assertException( /abc/, 0, 4 );

// Field path like string.
assertSubstring( '$a', 'a$a', 1, 2 );

// Multi byte utf-8.
assertSubstring( '\u0080', '\u0080', 0, 2 );
if ( 0 ) { // SERVER-6801
assertException( '\u0080', 0, 1 );
assertException( '\u0080', 1, 1 );
}
assertSubstring( '\u0080', '\u0080\u20ac', 0, 2 );
assertSubstring( '\u20ac', '\u0080\u20ac', 2, 3 );
if ( 0 ) { // SERVER-6801
assertException( '\u0080\u20ac', 1, 3 );
assertException( '\u0080\u20ac', 1, 4 );
assertException( '\u0080\u20ac', 0, 3 );
}
assertSubstring( '\u0044\u20ac', '\u0080\u0044\u20ac', 2, 4 );
assertSubstring( '\u0044', '\u0080\u0044\u20ac', 2, 1 );

// Operands from document.
t.drop();
t.save( { x:'a', y:'abc', z:'abcde', a:0, b:1, c:2, d:3, e:4, f:5 } );
assertSubstring( 'a', '$x', '$a', '$b' );
assertSubstring( 'a', '$x', '$a', '$f' );
assertSubstring( 'b', '$y', '$b', '$b' );
assertSubstring( 'b', '$z', '$b', '$b' );
assertSubstring( 'bcd', '$z', '$b', '$d' );
assertSubstring( 'cde', '$z', '$c', '$f' );
assertSubstring( 'c', '$y', '$c', '$f' );

// Computed operands.
assertSubstring( 'cde', '$z', { $add:[ '$b', '$b' ] }, { $add:[ '$c', '$d' ] } );
assertSubstring( 'cde', '$z', { $add:[ '$b', 1 ] }, { $add:[ 2, '$d' ] } );

// Nested.
assert.eq( 'e',
          t.aggregate( { $project:{ a:
                      { $substr:
                       [ { $substr:
                        [ { $substr:
                         [ { $substr:[ 'abcdefghij', 1, 6 ]
                          }, 2, 5 ]
                         }, 0, 3 ]
                        }, 1, 1 ]
                      } } } ).result[ 0 ].a ); 
