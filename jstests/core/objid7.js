
a = new ObjectId( "4c1a478603eba73620000000" )
b = new ObjectId( "4c1a478603eba73620000000" )
c = new ObjectId();

assert.eq( a.toString() , b.toString() , "A" )
assert.eq( a.toString()  , "ObjectId(\"4c1a478603eba73620000000\")" , "B" );

assert( a.equals( b ) , "C" )

assert.neq( a.toString() , c.toString() , "D" );
assert( ! a.equals( c ) , "E" );

