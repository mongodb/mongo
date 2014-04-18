assert.eq.automsg( "0", "new NumberLong()" );

n = new NumberLong( 4 );
assert.eq.automsg( "4", "n" );
assert.eq.automsg( "4", "n.toNumber()" );
assert.eq.automsg( "8", "n + 4" );
assert.eq.automsg( "'NumberLong(4)'", "n.toString()" );
assert.eq.automsg( "'NumberLong(4)'", "tojson( n )" );
a = {}
a.a = n;
p = tojson( a );
assert.eq.automsg( "'{ \"a\" : NumberLong(4) }'", "p" );

assert.eq.automsg( "NumberLong(4 )", "eval( tojson( NumberLong( 4 ) ) )" );
assert.eq.automsg( "a", "eval( tojson( a ) )" );

n = new NumberLong( -4 );
assert.eq.automsg( "-4", "n" );
assert.eq.automsg( "-4", "n.toNumber()" );
assert.eq.automsg( "0", "n + 4" );
assert.eq.automsg( "'NumberLong(-4)'", "n.toString()" );
assert.eq.automsg( "'NumberLong(-4)'", "tojson( n )" );
a = {}
a.a = n;
p = tojson( a );
assert.eq.automsg( "'{ \"a\" : NumberLong(-4) }'", "p" );

// too big to fit in double
n = new NumberLong( "11111111111111111" );
assert.eq.automsg( "11111111111111112", "n.toNumber()" );
assert.eq.automsg( "11111111111111116", "n + 4" );
assert.eq.automsg( "'NumberLong(\"11111111111111111\")'", "n.toString()" );
assert.eq.automsg( "'NumberLong(\"11111111111111111\")'", "tojson( n )" );
a = {}
a.a = n;
p = tojson( a );
assert.eq.automsg( "'{ \"a\" : NumberLong(\"11111111111111111\") }'", "p" );

assert.eq.automsg( "NumberLong('11111111111111111' )", "eval( tojson( NumberLong( '11111111111111111' ) ) )" );
assert.eq.automsg( "a", "eval( tojson( a ) )" );

n = new NumberLong( "-11111111111111111" );
assert.eq.automsg( "-11111111111111112", "n.toNumber()" );
assert.eq.automsg( "-11111111111111108", "n + 4" );
assert.eq.automsg( "'NumberLong(\"-11111111111111111\")'", "n.toString()" );
assert.eq.automsg( "'NumberLong(\"-11111111111111111\")'", "tojson( n )" );
a = {}
a.a = n;
p = tojson( a );
assert.eq.automsg( "'{ \"a\" : NumberLong(\"-11111111111111111\") }'", "p" );

// parsing
assert.throws.automsg( function() { new NumberLong( "" ); } );
assert.throws.automsg( function() { new NumberLong( "y" ); } );
assert.throws.automsg( function() { new NumberLong( "11111111111111111111" ); } );
