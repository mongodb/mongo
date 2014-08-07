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

// parsing: v8 evaluates not numbers to 0 which is not bad
//assert.throws.automsg( function() { new NumberInt( "" ); } );
//assert.throws.automsg( function() { new NumberInt( "y" ); } );

// eq

assert.eq( { x : 5 } , { x : new NumberInt( "5" ) } );

assert( 5 == NumberInt( 5 ) , "eq" );
assert( 5 < NumberInt( 6 ) , "lt" );
assert( 5 > NumberInt( 4 ) , "lt" );
assert( NumberInt( 1 ) , "to bool a" );

// objects are always considered thruthy
//assert( ! NumberInt( 0 ) , "to bool b" );

// create doc with int value in db
t = db.getCollection( "numberint" );
t.drop();

o = { a : NumberInt(42) };
t.save( o );

assert.eq( 42 , t.findOne().a , "save doc 1" );
assert.eq( 1 , t.find({a: {$type: 16}}).count() , "save doc 2" );
assert.eq( 0 , t.find({a: {$type: 1}}).count() , "save doc 3" );

// roundtripping
mod = t.findOne({a: 42});
mod.a += 10;
mod.b = "foo";
delete mod._id;
t.save(mod);
assert.eq( 2 , t.find({a: {$type: 16}}).count() , "roundtrip 1" );
assert.eq( 0 , t.find({a: {$type: 1}}).count() , "roundtrip 2" );
assert.eq( 1 , t.find({a: 52}).count() , "roundtrip 3" );

// save regular number
t.save({a: 42});
assert.eq( 2 , t.find({a: {$type: 16}}).count() , "normal 1" );
assert.eq( 1 , t.find({a: {$type: 1}}).count() , "normal 2" );
assert.eq( 2 , t.find({a: 42}).count() , "normal 3" );


