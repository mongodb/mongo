assert.eq.automsg( "0", "new NumberInt()" );

n = new NumberInt( 4 );
assert.eq.automsg( "4", "n" );
assert.eq.automsg( "4", "n.toNumber()" );
assert.eq.automsg( "8", "n + 4" );
assert.eq.automsg( "'NumberInt(4)'", "n.toString()" );
assert.eq.automsg( "'NumberInt(4)'", "tojson( n )" );
a = {}
a.a = n;
p = tojson( a );
assert.eq.automsg( "'{ \"a\" : NumberInt(4) }'", "p" );

assert.eq.automsg( "NumberInt(4 )", "eval( tojson( NumberInt( 4 ) ) )" );
assert.eq.automsg( "a", "eval( tojson( a ) )" );

n = new NumberInt( -4 );
assert.eq.automsg( "-4", "n" );
assert.eq.automsg( "-4", "n.toNumber()" );
assert.eq.automsg( "0", "n + 4" );
assert.eq.automsg( "'NumberInt(-4)'", "n.toString()" );
assert.eq.automsg( "'NumberInt(-4)'", "tojson( n )" );
a = {}
a.a = n;
p = tojson( a );
assert.eq.automsg( "'{ \"a\" : NumberInt(-4) }'", "p" );

n = new NumberInt( "11111" );
assert.eq.automsg( "'NumberInt(11111)'", "n.toString()" );
assert.eq.automsg( "'NumberInt(11111)'", "tojson( n )" );
a = {}
a.a = n;
p = tojson( a );
assert.eq.automsg( "'{ \"a\" : NumberInt(11111) }'", "p" );

assert.eq.automsg( "NumberInt('11111' )", "eval( tojson( NumberInt( '11111' ) ) )" );
assert.eq.automsg( "a", "eval( tojson( a ) )" );

n = new NumberInt( "-11111" );
assert.eq.automsg( "-11111", "n.toNumber()" );
assert.eq.automsg( "-11107", "n + 4" );
assert.eq.automsg( "'NumberInt(-11111)'", "n.toString()" );
assert.eq.automsg( "'NumberInt(-11111)'", "tojson( n )" );
a = {}
a.a = n;
p = tojson( a );
assert.eq.automsg( "'{ \"a\" : NumberInt(-11111) }'", "p" );

// parsing
assert.throws.automsg( function() { new NumberInt( "" ); } );
assert.throws.automsg( function() { new NumberInt( "y" ); } );

// eq

assert.eq( { x : 5 } , { x : new NumberInt( "5" ) } );
