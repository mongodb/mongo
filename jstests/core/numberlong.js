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

// create NumberLong from NumberInt (SERVER-9973)
assert.doesNotThrow.automsg( function() { new NumberLong(NumberInt(1)); } );

// check that creating a NumberLong from a NumberLong bigger than a double doesn't
// get a truncated value (SERVER-9973)
n = new NumberLong(NumberLong( "11111111111111111" ));
assert.eq.automsg("n.toString()", "'NumberLong(\"11111111111111111\")'");

//
// Test NumberLong.compare()
//

var left = new NumberLong("0");
var right = new NumberLong("0");
assert.eq(left.compare(right), 0);
assert.eq(right.compare(left), 0);

left = new NumberLong("20");
right = new NumberLong("10");
assert.gt(left.compare(right), 0);
assert.lt(right.compare(left), 0);

left = new NumberLong("-9223372036854775808");
right = new NumberLong("9223372036854775807");
assert.lt(left.compare(right), 0);
assert.gt(right.compare(left), 0);
assert.eq(left.compare(left), 0);
assert.eq(right.compare(right), 0);

// Bad input to .compare().
assert.throws(function() { NumberLong("0").compare(); });
assert.throws(function() { NumberLong("0").compare(null); });
assert.throws(function() { NumberLong("0").compare(undefined); });
assert.throws(function() { NumberLong("0").compare(3); });
assert.throws(function() { NumberLong("0").compare("foo"); });
assert.throws(function() { NumberLong("0").compare(NumberLong("0"), 3); });
